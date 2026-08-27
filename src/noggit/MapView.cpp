// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#include <noggit/AssetScan.hpp>
#include <noggit/MissingPlacementLog.hpp>
#include <noggit/UidCollisionLog.hpp>
#include <noggit/ui/tools/MissingObjects/MissingObjectsPanel.hpp>
#include <noggit/database/ChangesetBuilder.hpp>
#include <noggit/database/DatabaseSettings.hpp>
#include <noggit/database/GameTeleBuilder.hpp>
#include <noggit/database/SchemaIntrospector.hpp>
#include <noggit/database/SpawnQuery.hpp>
#include <noggit/database/SpawnSceneCache.hpp>
#include <noggit/database/TileCoordinates.hpp>
#include <noggit/database/WorldDatabaseConnection.hpp>
#include <noggit/database/SpawnPlacement.hpp>
#include <noggit/DBC.h>
#include <noggit/MapChunk.h>
#include <noggit/MapView.h>
#include <noggit/Misc.h>
#include <noggit/ModelManager.h> // ModelManager
#include <noggit/TextureManager.h> // TextureManager, Texture
#include <noggit/WMOInstance.h> // WMOInstance
#include <noggit/World.h>
#include <noggit/MapTile.h>
#include <noggit/map_index.hpp>
#include <noggit/TabletManager.hpp>
#include <opengl/texture.hpp>
#include <noggit/Tool.hpp>
#include <noggit/uid_storage.hpp>
#include <noggit/ui/AlphaIntegrityReport.hpp>
#include <noggit/ui/AmbientOcclusionDialog.hpp>
#include <noggit/ui/AutoTextureDialog.hpp>
#include <noggit/ui/CurrentTexture.h>
#include <noggit/ui/DatabaseSpawnPanel.hpp>
#include <noggit/ui/GroundEffectSetEditor.hpp>
#include <noggit/ui/DetailInfos.h> // detailInfos
#include <noggit/ui/FlattenTool.hpp>
#include <noggit/ui/Help.h>
#include <noggit/ui/HelperModels.h>
#include <noggit/ui/ModelImport.h>
#include <noggit/ui/ObjectEditor.h>
#include <noggit/ui/RotationEditor.h>
#include <noggit/ui/TexturePicker.h>
#include <noggit/ui/TexturingGUI.h>
#include <noggit/ui/Toolbar.h> // Noggit::Ui::toolbar
#include <noggit/ui/Water.h>
#include <noggit/ui/ZoneIDBrowser.h>
#include <noggit/ui/windows/noggitWindow/NoggitWindow.hpp>
#include <noggit/ui/minimap_widget.hpp>
#include <noggit/ui/ShaderTool.hpp>
#include <noggit/ui/texture_swapper.hpp>
#include <noggit/ui/texturing_tool.hpp>
#include <noggit/ui/GroundEffectsTool.hpp>
#include <noggit/ui/hole_tool.hpp>
#include <noggit/ui/texture_palette_small.hpp>
#include <noggit/ui/MinimapCreator.hpp>
#include <noggit/project/CurrentProject.hpp>
#include <opengl/scoped.hpp>
#include <noggit/ui/tools/LightEditor/LightEditor.hpp>
#include <noggit/ui/tools/ViewToolbar/Ui/ViewToolbar.hpp>
#include <noggit/ui/tools/AssetBrowser/Ui/AssetBrowser.hpp>
#include <noggit/ui/tools/PresetEditor/Ui/PresetEditor.hpp>
#include <noggit/ui/tools/NodeEditor/Ui/NodeEditor.hpp>
#include <noggit/ui/tools/UiCommon/ImageBrowser.hpp>
#include <noggit/ui/tools/BrushStack/BrushStack.hpp>
#include <noggit/ui/tools/LightEditor/LightEditor.hpp>
#include <noggit/ui/tools/ToolPanel/ToolPanel.hpp>
#include <noggit/ui/tools/ChunkManipulator/ChunkManipulatorPanel.hpp>
#include <external/imguipiemenu/PieMenu.hpp>
#include <external/tracy/Tracy.hpp>
#include <noggit/ui/object_palette.hpp>
#include <external/glm/gtc/type_ptr.hpp>
#include <external/qtimgui/QtImGui.h>
#include <opengl/types.hpp>
#include <limits>
#include <variant>
#include <noggit/Selection.h>
#include <noggit/ui/FontAwesome.hpp>

#include <noggit/Input.hpp>
#include <noggit/ToolDrawParameters.hpp>
#include <noggit/tools/RaiseLowerTool.hpp>
#include <noggit/tools/FlattenBlurTool.hpp>
#include <noggit/tools/TexturingTool.hpp>
#include <noggit/tools/HoleTool.hpp>
#include <noggit/tools/AreaTool.hpp>
#include <noggit/tools/ImpassTool.hpp>
#include <noggit/tools/WaterTool.hpp>
#include <noggit/tools/VertexPainterTool.hpp>
#include <noggit/tools/ObjectTool.hpp>
#include <noggit/tools/MinimapTool.hpp>
#include <noggit/tools/StampTool.hpp>
#include <noggit/tools/LightTool.hpp>
#include <noggit/tools/ScriptingTool.hpp>
#include <noggit/tools/ChunkTool.hpp>
#include <noggit/tools/AreaTriggerTool.hpp>
#include <noggit/tools/ErosionTool.hpp>
#include <noggit/StringHash.hpp>
#include <noggit/application/NoggitApplication.hpp>

#ifdef USE_MYSQL_UID_STORAGE
#include <mysql/mysql.h>

#endif
#include <QtCore/QDir>
#include <QtCore/QSettings>

#include <noggit/scripting/scripting_tool.hpp>
#include <noggit/scripting/script_settings.hpp>

#include <noggit/ActionManager.hpp>
#include <noggit/Action.hpp>

#include <noggit/ui/FontNoggit.hpp>

#include <ui_MapViewOverlay.h>


#include <QtCore/QTimer>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QOpenGLWidget>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStatusBar>
#include <QWidgetAction>
#include <QSurfaceFormat>
#include <QMessageBox>
#include <QAbstractScrollArea>
#include <QScrollBar>
#include <QDateTime>
#include <QCursor>
#include <QFileDialog>
#include <QProgressDialog>
#include <QClipboard>
#include <QOpenGLContext>
#include <QProcess>
#include <QWidgetAction>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <vector>
#include <random>
#include <format>


/* Some ugly macros we use */
// TODO: make those methods instead???

#define DESTRUCTIVE_ACTION(ACTION_CODE)                                                                                \
QMessageBox::StandardButton reply;                                                                                     \
reply = QMessageBox::question(this, "Destructive action", "This action cannot be undone. Current change history will be lost. Continue?", \
QMessageBox::Yes|QMessageBox::No);                                                                                     \
if (reply == QMessageBox::Yes)                                                                                         \
{                                                                                                                      \
NOGGIT_ACTION_MGR->purge();                                                                            \
ACTION_CODE                                                                                                            \
}                                                                                                                      \

// add action no shortcut
#define ADD_ACTION_NS(menu, name, on_action)                      \
  {                                                               \
    auto action (menu->addAction (name));                         \
    connect (action, &QAction::triggered, on_action);             \
  }


#define ADD_TOGGLE(menu_, name_, shortcut_, property_)            \
  do                                                              \
  {                                                               \
    QAction* action (new QAction (name_, this));                  \
    action->setShortcut (QKeySequence (shortcut_));               \
    action->setCheckable (true);                                  \
    action->setChecked (property_.get());                         \
    menu_->addAction (action);                                    \
    connect ( action, &QAction::toggled                           \
            , &property_, &Noggit::BoolToggleProperty::set      \
            );                                                    \
    connect ( &property_, &Noggit::BoolToggleProperty::changed  \
            , action, &QAction::setChecked                        \
            );                                                    \
  }                                                               \
  while (false)


#define ADD_TOGGLE_NS(menu_, name_, property_)                    \
  do                                                              \
  {                                                               \
    QAction* action (new QAction (name_, this));                  \
    action->setCheckable (true);                                  \
    action->setChecked (property_.get());                         \
    menu_->addAction (action);                                    \
    connect ( action, &QAction::toggled                           \
            , &property_, &Noggit::BoolToggleProperty::set      \
            );                                                    \
    connect ( &property_, &Noggit::BoolToggleProperty::changed  \
            , action, &QAction::setChecked                        \
            );                                                    \
  }                                                               \
  while (false)


#define ADD_TOGGLE_POST(menu_, name_, shortcut_, property_, post_)\
  do                                                              \
  {                                                               \
    QAction* action (new QAction (name_, this));                  \
    action->setShortcut (QKeySequence (shortcut_));               \
    action->setCheckable (true);                                  \
    action->setChecked (property_.get());                         \
    menu_->addAction (action);                                    \
    connect ( action, &QAction::toggled                           \
            , &property_, &Noggit::BoolToggleProperty::set      \
            );                                                    \
    connect ( &property_, &Noggit::BoolToggleProperty::changed  \
            , action, &QAction::setChecked                        \
            );                                                    \
    connect ( action, &QAction::toggled, post_);                  \
    connect ( &property_, &Noggit::BoolToggleProperty::changed, \
    post_);                                                       \
  }                                                               \
  while (false)



#define ADD_TOGGLE_NS_POST(menu_, name_, property_, code_)        \
  do                                                              \
  {                                                               \
    QAction* action (new QAction (name_, this));                  \
    action->setCheckable (true);                                  \
    action->setChecked (property_.get());                         \
    menu_->addAction (action);                                    \
    connect ( action, &QAction::toggled                           \
            , &property_, &Noggit::bool_toggle_property::set      \
            );                                                    \
    connect ( &property_, &Noggit::bool_toggle_property::changed  \
            , action, &QAction::setChecked                        \
            );                                                    \
      connect ( action, &QAction::toggled                         \
            ,  code_                                              \
            );                                                    \
    connect ( &property_, &Noggit::bool_toggle_property::changed  \
            , code_                                               \
            );                                                    \
  }                                                               \
  while (false)



#define ADD_ACTION(menu, name, shortcut, on_action)               \
  {                                                               \
    auto action (menu->addAction (name));                         \
    action->setShortcut (QKeySequence (shortcut));                \
    auto callback = on_action;                                    \
    connect (action, &QAction::triggered, [this, callback]()      \
    {                                                             \
       if (NOGGIT_CUR_ACTION) \
        return;                                                   \
       callback();                                                \
                                                                  \
    });                                                           \
  }

using Noggit::XSENS;
using Noggit::YSENS;

void MapView::set_editing_mode(editing_mode mode)
{

  {
    QSignalBlocker const asset_browser_blocker(_asset_browser_dock);

    _asset_browser_dock->hide();
    _viewport_overlay_ui->gizmoBar->hide();
  }

  auto previous_mode = _left_sec_toolbar->getCurrentMode();

  _left_sec_toolbar->setCurrentMode(this, mode);

  // hack to hide empty tools
  if (mode == editing_mode::impass)
  {
    _tool_panel_dock->hide();
  }
  else
  {
    _tool_panel_dock->show();
  }

  if (context() && context()->isValid())
  {
    _world->renderer()->getTerrainParamsUniformBlock()->draw_areaid_overlay = false;
    _world->renderer()->getTerrainParamsUniformBlock()->draw_impass_overlay = false;
    _world->renderer()->getTerrainParamsUniformBlock()->draw_paintability_overlay = false;
    _world->renderer()->getTerrainParamsUniformBlock()->draw_selection_overlay = false;
    _world->renderer()->getTerrainParamsUniformBlock()->draw_groundeffectid_overlay = false;
    _world->renderer()->getTerrainParamsUniformBlock()->draw_groundeffect_layerid_overlay = false;
    _world->renderer()->getTerrainParamsUniformBlock()->draw_noeffectdoodad_overlay = false;
    _world->renderer()->getTerrainParamsUniformBlock()->draw_only_normals = false;
    _world->renderer()->getTerrainParamsUniformBlock()->point_normals_up = false;
    _minimap->use_selection(nullptr);
    
    if (terrainMode != mode)
    {
        activeTool()->onDeselected();
        activeTool(mode);
        activeTool()->onSelected();
    }
  }

  _world->reset_selection();
  emit rotationChanged();

  if (!ui_hidden)
  {
    setToolPropertyWidgetVisibility(mode);
  }

  terrainMode = mode;
  _toolbar->check_tool (mode);
  this->activateWindow();

  _tool_panel_dock->setWindowTitle(activeTool()->name());

  _world->renderer()->markTerrainParamsUniformBlockDirty();
}

editing_mode MapView::get_editing_mode() const
{
  return terrainMode;
}

void MapView::setToolPropertyWidgetVisibility(editing_mode mode)
{
  _tool_panel_dock->setCurrentTool(mode);

  switch (mode)
  {

  case editing_mode::object:
    _asset_browser_dock->setVisible(!ui_hidden && _settings->value("map_view/asset_browser", false).toBool());
    _viewport_overlay_ui->gizmoBar->setVisible(!ui_hidden);
    break;
  default:
    break;
  }

  
}

void MapView::ResetSelectedObjectRotation()
{
  if (terrainMode != editing_mode::object)
  {
    return;
  }

  for (auto& selection : _world->current_selection())
  {
    if (selection.index() != eEntry_Object)
      continue;

    auto obj = std::get<selected_object_type>(selection);

    if (obj->which() == eWMO)
    {
      WMOInstance* wmo = static_cast<WMOInstance*>(obj);
      _world->updateTilesWMO(wmo, model_update::remove);
      wmo->resetDirection();
      wmo->recalcExtents();
      _world->updateTilesWMO(wmo, model_update::add);
    }
    else if (obj->which() == eMODEL)
    {
      ModelInstance* m2 = static_cast<ModelInstance*>(obj);
      _world->updateTilesModel(m2, model_update::remove);
      m2->resetDirection();
      m2->recalcExtents();
      _world->updateTilesModel(m2, model_update::add);
    }
  }

  emit rotationChanged();
}

void MapView::snap_selected_models_to_the_ground()
{
  if (terrainMode != editing_mode::object)
  {
    return;
  }

  _world->snap_selected_models_to_the_ground();
  emit rotationChanged();
}

bool MapView::isRotatingCamera() const
{
    return look;
}


void MapView::DeleteSelectedObjects()
{
  if (terrainMode != editing_mode::object)
  {
    return;
  }

  makeCurrent();
  OpenGL::context::scoped_setter const _ (::gl, context());

  _world->delete_selected_models();
  emit rotationChanged();
}

QWidgetAction* MapView::createTextSeparator(const QString& text)
{
  auto* pLabel = new QLabel(text);
  //pLabel->setMinimumWidth(this->minimumWidth() - 4);
  pLabel->setAlignment(Qt::AlignCenter);
  auto* separator = new QWidgetAction(this);
  separator->setDefaultWidget(pLabel);
  return separator;
}

std::string MapView::reportDatabaseSpawnOutcome
  (std::string const& message, bool is_error, bool interactive)
{
  if (interactive)
  {
    if (is_error)
    {
      QMessageBox::critical(this, "Database spawns", QString::fromStdString(message));
    }
    else
    {
      QMessageBox::information(this, "Database spawns", QString::fromStdString(message));
    }
  }

  return (is_error ? std::string("ERR ") : std::string("OK ")) + message;
}

#ifdef USE_MYSQL_UID_STORAGE
Noggit::Database::SchemaModel const& MapView::databaseSchemaFor
  ( Noggit::Database::WorldDatabaseConnection const& connection
  , Noggit::Database::ConnectionConfig const& config
  , bool refresh
  )
{
  // connection.schema(), not config.schema, even though the connection was opened with that
  // config: the connection is the authority on what it is actually pointed at, and it is what
  // readModel is about to be handed below. Fingerprinting one and measuring the other is how a
  // cache comes to answer for a schema nobody read.
  std::string fingerprint (config.host);
  fingerprint += ':';
  fingerprint += std::to_string(config.port);
  fingerprint += '/';
  fingerprint += connection.schema();
  fingerprint += '@';
  fingerprint += config.user;

  if (!refresh && _db_schema && fingerprint == _db_schema_fingerprint)
  {
    return *_db_schema;
  }

  // Reset first, so a throw from readModel leaves no cached model at all rather than the previous
  // server's still answering under the new fingerprint.
  _db_schema.reset();
  _db_schema_fingerprint.clear();

  auto model
    ( std::make_unique<Noggit::Database::SchemaModel>
        (Noggit::Database::SchemaIntrospector::readModel(connection, connection.schema())) );

  _db_schema = std::move(model);
  _db_schema_fingerprint = std::move(fingerprint);

  Log << "Introspected schema \"" << connection.schema() << "\": "
      << _db_schema->tableCount() << " table(s), " << _db_schema->columnCount()
      << " column(s). Cached until the connection settings change or spawns are loaded again."
      << std::endl;

  return *_db_schema;
}
#endif

std::string MapView::loadDatabaseSpawns(bool all_loaded_tiles, bool interactive, bool force)
{
#ifdef USE_MYSQL_UID_STORAGE
  // Decides a tile set, and nothing else. Everything that loads -- connection, introspection,
  // unsaved-changes warning, pre-flight COUNT, the OpenGL context guard -- is in
  // loadDatabaseSpawnsForTiles, which the tile picker calls with a set of its own. Two copies of
  // that sequence would drift, and the half that drifts is the context guard, whose absence
  // terminates the process instead of failing.
  //
  // The feature check stays here rather than being left to the callee: with the database turned
  // off, "the tile under the camera is not loaded" is a confusing first thing to be told.
  if (!Noggit::Database::DatabaseSettings::isEnabled())
  {
    return reportDatabaseSpawnOutcome
      ( "The database feature is not enabled. Turn it on and set the connection details in "
        "Settings first."
      , true
      , interactive
      );
  }

  std::vector<::TileIndex> tiles;

  if (all_loaded_tiles)
  {
    for (MapTile* tile : _world->mapIndex.loaded_tiles())
    {
      if (tile)
      {
        tiles.push_back(tile->index);
      }
    }

    if (tiles.empty())
    {
      return reportDatabaseSpawnOutcome("No tiles are loaded.", true, interactive);
    }
  }
  else
  {
    ::TileIndex const current (_camera.position);

    // Only this path needs it, and that is the whole reason the requirement is expressed here: it
    // reads the camera's tile index, so the camera has to be over a tile. Nothing about querying
    // spawns by coordinate needs ADT geometry resident, and loadDatabaseSpawnsForTiles imposes no
    // such gate on the sets it is handed.
    if (!_world->mapIndex.tileLoaded(current))
    {
      return reportDatabaseSpawnOutcome
        ( "The tile under the camera is not loaded yet. Wait for it to finish streaming, or load "
          "all loaded tiles instead."
        , true
        , interactive
        );
    }

    tiles.push_back(current);
  }

  return loadDatabaseSpawnsForTiles(tiles, interactive, force);
#else
  (void)all_loaded_tiles;
  (void)force;

  if (interactive)
  {
    QMessageBox::information
      ( this
      , "Database spawns"
      , "This build has no database support. Reconfigure with -DUSE_SQL=ON."
      );
  }

  return "ERR this build has no database support (reconfigure with -DUSE_SQL=ON)";
#endif
}

std::string MapView::loadDatabaseSpawnsForTiles
  (std::vector<::TileIndex> const& tiles, bool interactive, bool force)
{
#ifdef USE_MYSQL_UID_STORAGE
  // Above this many spawns the load is worth confirming rather than simply starting. The cost is
  // not the query -- it is that each spawn builds a ModelInstance that queues an asynchronous M2
  // load, so a dense set of city tiles can queue thousands and present as a hang.
  constexpr std::size_t SPAWN_COUNT_CONFIRM_THRESHOLD = 2000;

  // Every exit reports through here, so the menu and the panel cannot drift into describing the
  // same outcome differently. A dialog is shown only when a human asked; a non-interactive caller
  // must never be left waiting on a modal nobody will click.
  auto const report = [this, interactive] (std::string const& message, bool is_error)
  {
    return reportDatabaseSpawnOutcome(message, is_error, interactive);
  };

  if (!Noggit::Database::DatabaseSettings::isEnabled())
  {
    return report("The database feature is not enabled. Turn it on and set the connection "
                  "details in Settings first.", true);
  }

  if (tiles.empty())
  {
    return report("No tiles were given to load.", true);
  }

  // Loading rebuilds the scene from scratch, which throws away every unsaved move and rotation.
  // Discard already asks before doing exactly that, so loading doing it silently was the
  // inconsistency -- and the more damaging half, because Discard is a button you press on purpose
  // while Load is one you press to see more.
  // pendingCount, not dirtyCount: placements and deletions are discarded by a reload exactly as
  // moves are, and counting only moves meant a session that had placed twenty spawns and moved
  // none was told there was nothing to lose.
  if (_db_spawn_scene && _db_spawn_scene->pendingCount() > 0)
  {
    std::string const warning
      ( std::to_string(_db_spawn_scene->pendingCount())
      + " unsaved spawn change(s) -- moves, placements and deletions. Loading discards them." );

    if (!interactive)
    {
      // A script gets refused rather than silently losing the edits it just made.
      return report(warning + " Save or discard first.", true);
    }

    auto const answer
      ( QMessageBox::question
        ( this
        , "Database spawns"
        , QString::fromStdString(warning) + "\n\nLoad anyway and lose them?"
        , QMessageBox::Yes | QMessageBox::No
        , QMessageBox::No
        )
      );

    if (answer != QMessageBox::Yes)
    {
      return "OK cancelled";
    }
  }

  // Everything below can throw -- the connection, the introspection, and every query. All of it
  // is contained here, because this runs from a Qt slot and an exception escaping a slot is
  // undefined behaviour under Qt, not a caught error.
  try
  {
    // READ_ONLY unconditionally. This path only ever reads, and asking for a write-capable
    // connection would make the layer refuse to construct against anything but the dev schema --
    // which would stop the overlay working against a real world database, the main thing it is
    // for. See HARD RULE 1: reads against a live schema are fine.
    auto const config (Noggit::Database::DatabaseSettings::readConnectionConfig());

    Noggit::Database::WorldDatabaseConnection connection
      ( config
      , Noggit::Database::AccessMode::READ_ONLY
      , Noggit::Database::DatabaseSettings::readWritableSchema()
      );

    // refresh true: this is the deliberate "go and read the database" action, it happens once per
    // user gesture rather than once per click, and it is therefore the natural place to pay for a
    // fresh introspection. Everything else -- placing a spawn, saving a changeset -- reuses what
    // this leaves cached. See databaseSchemaFor.
    Noggit::Database::SchemaModel const& schema (databaseSchemaFor(connection, config, true));

    auto const map_id (static_cast<std::uint16_t>(_world->getMapID()));

    // The frame change, in one place, BEFORE the cache is touched -- so a cancelled confirmation
    // leaves the existing overlay exactly as it was rather than cleared.
    //
    // fromAdtFileIndex, never field-by-field. Noggit's index is (x, z) in ADT filename order and
    // the database layer's is transposed; assigning one to the other reads a tile about 9.6 km
    // from the one on screen, with no error to show for it.
    auto const to_db_tile = [] (::TileIndex const& adt)
    {
      return Noggit::Database::fromAdtFileIndex
        ( Noggit::Database::AdtFileIndex
          {static_cast<int>(adt.x), static_cast<int>(adt.z)}
        );
    };

    std::vector<Noggit::Database::TileIndex> targets;
    targets.reserve(tiles.size());

    for (auto const& adt_tile : tiles)
    {
      targets.push_back(to_db_tile(adt_tile));
    }

    // Pre-flight count, for every tile set rather than only a multi-tile one. What the threshold
    // is about is how many ModelInstances are about to queue an asynchronous model load, which
    // does not depend on how the tile list was chosen -- and one dense city tile can exceed it on
    // its own. Two COUNT queries per tile with no joins and no rows fetched, which is cheap next
    // to the load it is describing.
    std::size_t expected = 0;

    for (auto const& tile : targets)
    {
      expected += Noggit::Database::SpawnQuery::countTile(connection, schema, map_id, tile);
    }

    if (expected > SPAWN_COUNT_CONFIRM_THRESHOLD && !force)
    {
      if (!interactive)
      {
        // Refused rather than silently loaded. A script asking for a large tile set against a
        // populated world database is exactly the case this threshold exists for, and it has no
        // human to warn -- so it has to say no and explain how to insist.
        return report(std::to_string(expected) + " spawns across " + std::to_string(targets.size())
                      + " tile(s) exceeds the confirmation threshold. Repeat with force to load "
                        "them anyway.", true);
      }

      auto const answer
        ( QMessageBox::question
          ( this
          , "Database spawns"
          , QString("%1 spawns across %2 tile(s).\n\nEach one queues a model load, so "
                    "this may take a while and use a lot of memory. Continue?")
              .arg(expected).arg(targets.size())
          , QMessageBox::Yes | QMessageBox::No
          , QMessageBox::No
          )
        );

      if (answer != QMessageBox::Yes)
      {
        Log << "Database spawn load cancelled by the user: " << expected << " spawn(s) across "
            << targets.size() << " tile(s)." << std::endl;

        return "OK cancelled";
      }
    }

    if (!_db_spawn_scene)
    {
      _db_spawn_scene = std::make_unique<Noggit::Database::SpawnSceneCache>(getRenderContext());
    }

    // An OpenGL context must be current for the rest of this function, and the reason is not
    // obvious enough to leave unstated.
    //
    // Every entry in the cache owns a scoped_model_reference. Releasing the last reference to a
    // Model runs Model::~Model, which destroys its ModelRender, which destroys OpenGL vertex
    // array objects -- and OpenGL::Scoped's destructor calls verify_context_and_check_for_gl_errors,
    // which THROWS when no context is current (context.inl:47). Thrown from a destructor, that is
    // an immediate terminate, not a catchable error: exactly the crash observed on the second
    // invocation of this action, where clear() below released the first run's models from a plain
    // Qt slot with no context bound.
    //
    // This is the same guard, for the same reason, that ~MapView applies before deleting its
    // tools -- see the "opengl context related crash" comment there. Both clear() and setTile()
    // can release the last reference to a model, so the context covers both.
    makeCurrent();
    OpenGL::context::scoped_setter const _gl_context (::gl, context());

    // Rebuilt from scratch rather than merged: tiles unloaded since the last run must not keep
    // stale spawns alive, and the resolver cache -- the expensive part -- survives regardless
    // because it belongs to the cache, not to this call.
    _db_spawn_scene->clear();

    for (auto const& db_tile : targets)
    {
      _db_spawn_scene->setTile
        (Noggit::Database::SpawnQuery::loadTile(connection, schema, map_id, db_tile));
    }

    std::size_t const tiles_read = targets.size();

    // Turned on for the user: having loaded spawns on request and then not shown them would read
    // as the load having failed.
    _draw_db_spawns.set(true);
    markSpawnOverlayDirty();

    std::string const summary (_db_spawn_scene->summary());
    Log << "Database spawns loaded from schema \"" << connection.schema() << "\" over "
        << tiles_read << " tile(s): " << summary << std::endl;

    _main_window->statusBar()->showMessage
      (QString::fromStdString("Database spawns: " + summary), 5000);

    // Success is not reported through `report`: it must not raise a dialog on the menu path, where
    // the status bar has already said it.
    return "OK " + summary;
  }
  catch (std::exception const& e)
  {
    LogError << "Loading database spawns failed: " << e.what() << std::endl;

    return report(std::string("Could not load spawns. ") + e.what(), true);
  }
#else
  (void)tiles;
  (void)force;

  if (interactive)
  {
    QMessageBox::information
      ( this
      , "Database spawns"
      , "This build has no database support. Reconfigure with -DUSE_SQL=ON."
      );
  }

  return "ERR this build has no database support (reconfigure with -DUSE_SQL=ON)";
#endif
}

#ifdef USE_MYSQL_UID_STORAGE
namespace
{
  // The template lookup behind spawn creation.
  //
  // Kept here rather than in SpawnQuery because it is the only query in the project that starts
  // from an entry id instead of from a tile, and because it exists to serve one interactive
  // action. Every table and column it names is checked against the introspected schema first --
  // HARD RULE 3 -- and it refuses rather than degrading: a create that silently resolved to no
  // model would place a spawn the user cannot see, which is the failure this whole path is
  // guarding against.

  constexpr char const* TABLE_CREATURE_TEMPLATE = "creature_template";
  constexpr char const* TABLE_CREATURE_TEMPLATE_MODEL = "creature_template_model";
  constexpr char const* TABLE_GAMEOBJECT_TEMPLATE = "gameobject_template";

  constexpr char const* COLUMN_ENTRY = "entry";
  constexpr char const* COLUMN_NAME = "name";
  constexpr char const* COLUMN_DISPLAY_ID = "displayId";
  constexpr char const* COLUMN_TYPE = "type";

  // creature_template_model, where a core has it instead of modelid1..4.
  constexpr char const* COLUMN_CREATURE_ID = "CreatureID";
  constexpr char const* COLUMN_CREATURE_DISPLAY_ID = "CreatureDisplayID";
  constexpr char const* COLUMN_IDX = "Idx";

  // Four, matching SpawnQueryDetail's MODEL_CANDIDATE_COUNT and the cap modelid1..4 imposes. The
  // count only ever has to answer "was there more than one".
  constexpr std::size_t MODEL_CANDIDATE_COUNT = 4;

  std::string quotedIdentifier(std::string const& name)
  {
    return "`" + name + "`";
  }

  void requireColumn
    (Noggit::Database::SchemaModel const& schema, char const* table, char const* column)
  {
    if (!schema.hasColumn(table, column))
    {
      throw Noggit::Database::SchemaCapabilityError
        (std::string(table) + " has no " + column + " column, so an entry id cannot be resolved"
         " to a model on this schema.");
    }
  }

  std::string rowField(Noggit::Database::ResultRow const& row, std::size_t index)
  {
    return index < row.size() ? row[index] : std::string();
  }

  std::uint32_t rowUnsigned(Noggit::Database::ResultRow const& row, std::size_t index)
  {
    std::string const text (rowField(row, index));

    if (text.empty())
    {
      return 0;
    }

    try
    {
      unsigned long long const value (std::stoull(text));

      // Saturating, not wrapping. A value too wide for the column has to stay obviously wrong
      // rather than becoming a small number that names a different display id.
      return value > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(value);
    }
    catch (std::exception const&)
    {
      return 0;
    }
  }

  // creature_template row for one entry: name plus up to four display id candidates, in the
  // template's own order. Resolution of the candidates into a single display id is
  // SpawnDisplay::resolveCreatureTemplateInfo's job, exactly as it is on the read path.
  Noggit::Database::CreatureTemplateInfo creatureTemplateFor
    ( Noggit::Database::WorldDatabaseConnection const& connection
    , Noggit::Database::SchemaModel const& schema
    , std::uint32_t entry
    )
  {
    if (!schema.hasTable(TABLE_CREATURE_TEMPLATE))
    {
      throw Noggit::Database::SchemaCapabilityError("there is no creature_template table");
    }

    requireColumn(schema, TABLE_CREATURE_TEMPLATE, COLUMN_ENTRY);

    // Which columns hold the models is a schema question, and SchemaModel is the single authority
    // on it -- the published references claim creature_template_model coexists with modelid1..4
    // on 3.3.5, and it does not. Asking here rather than probing keeps one copy of that rule.
    bool const from_model_table
      ( schema.creatureModelSource()
          == Noggit::Database::CreatureModelSource::TEMPLATE_MODEL_TABLE );

    std::string select (schema.hasColumn(TABLE_CREATURE_TEMPLATE, COLUMN_NAME)
                          ? quotedIdentifier(COLUMN_NAME)
                          : std::string("''"));

    if (!from_model_table)
    {
      for (std::size_t i = 1; i <= MODEL_CANDIDATE_COUNT; ++i)
      {
        std::string const column ("modelid" + std::to_string(i));

        // A missing slot degrades to a literal 0 rather than failing the query: modelid3 and
        // modelid4 are absent on some generations and their absence is not an error, it is a
        // template with fewer alternatives.
        select += ", " + (schema.hasColumn(TABLE_CREATURE_TEMPLATE, column)
                            ? quotedIdentifier(column)
                            : std::string("0"));
      }
    }

    auto const rows
      ( connection.query
          ( "SELECT " + select + " FROM " + quotedIdentifier(TABLE_CREATURE_TEMPLATE)
          + " WHERE " + quotedIdentifier(COLUMN_ENTRY) + " = " + std::to_string(entry)
          + " LIMIT 1"
          )
      );

    if (rows.empty())
    {
      throw std::runtime_error
        ("there is no creature_template row with entry " + std::to_string(entry) + ".");
    }

    std::vector<std::uint32_t> candidates;

    if (from_model_table)
    {
      requireColumn(schema, TABLE_CREATURE_TEMPLATE_MODEL, COLUMN_CREATURE_ID);
      requireColumn(schema, TABLE_CREATURE_TEMPLATE_MODEL, COLUMN_CREATURE_DISPLAY_ID);

      // ORDER BY, never a bare LIMIT: an unordered result may legitimately come back in a
      // different order each time, and a spawn that draws a different model on each placement is
      // unusable. Idx where the schema has it, the display id itself where it does not --
      // arbitrary but stable beats meaningful but nondeterministic.
      std::string const order
        ( schema.hasColumn(TABLE_CREATURE_TEMPLATE_MODEL, COLUMN_IDX)
            ? COLUMN_IDX
            : COLUMN_CREATURE_DISPLAY_ID
        );

      for (auto const& row : connection.query
             ( "SELECT " + quotedIdentifier(COLUMN_CREATURE_DISPLAY_ID)
             + " FROM " + quotedIdentifier(TABLE_CREATURE_TEMPLATE_MODEL)
             + " WHERE " + quotedIdentifier(COLUMN_CREATURE_ID) + " = " + std::to_string(entry)
             + " AND " + quotedIdentifier(COLUMN_CREATURE_DISPLAY_ID) + " <> 0"
             + " ORDER BY " + quotedIdentifier(order)
             + " LIMIT " + std::to_string(MODEL_CANDIDATE_COUNT)
             ))
      {
        candidates.push_back(rowUnsigned(row, 0));
      }
    }
    else
    {
      for (std::size_t i = 0; i < MODEL_CANDIDATE_COUNT; ++i)
      {
        candidates.push_back(rowUnsigned(rows.front(), i + 1));
      }
    }

    // 0 for the spawn's own modelid: a newly placed creature has none, so the template decides.
    // Through the same pure rule the reader uses, so a created spawn and the identical row after
    // a reload resolve to the same model.
    return Noggit::Database::SpawnDisplay::resolveCreatureTemplateInfo
      (0, candidates, rowField(rows.front(), 0));
  }

  Noggit::Database::GameObjectTemplateInfo gameObjectTemplateFor
    ( Noggit::Database::WorldDatabaseConnection const& connection
    , Noggit::Database::SchemaModel const& schema
    , std::uint32_t entry
    )
  {
    if (!schema.hasTable(TABLE_GAMEOBJECT_TEMPLATE))
    {
      throw Noggit::Database::SchemaCapabilityError("there is no gameobject_template table");
    }

    // Both required, not optional. `gameobject` has no per-spawn display column at all, so
    // without displayId there is no model for a gameobject anywhere, and without type there is
    // no way to tell an invisible-by-nature entry from a broken one.
    requireColumn(schema, TABLE_GAMEOBJECT_TEMPLATE, COLUMN_ENTRY);
    requireColumn(schema, TABLE_GAMEOBJECT_TEMPLATE, COLUMN_DISPLAY_ID);
    requireColumn(schema, TABLE_GAMEOBJECT_TEMPLATE, COLUMN_TYPE);

    std::string const name_column
      ( schema.hasColumn(TABLE_GAMEOBJECT_TEMPLATE, COLUMN_NAME)
          ? quotedIdentifier(COLUMN_NAME)
          : std::string("''")
      );

    auto const rows
      ( connection.query
          ( "SELECT " + quotedIdentifier(COLUMN_DISPLAY_ID) + ", "
          + quotedIdentifier(COLUMN_TYPE) + ", " + name_column
          + " FROM " + quotedIdentifier(TABLE_GAMEOBJECT_TEMPLATE)
          + " WHERE " + quotedIdentifier(COLUMN_ENTRY) + " = " + std::to_string(entry)
          + " LIMIT 1"
          )
      );

    if (rows.empty())
    {
      throw std::runtime_error
        ("there is no gameobject_template row with entry " + std::to_string(entry) + ".");
    }

    Noggit::Database::GameObjectTemplateInfo info;
    info.display_id = rowUnsigned(rows.front(), 0);
    info.type = rowUnsigned(rows.front(), 1);
    info.name = rowField(rows.front(), 2);

    return info;
  }
}
#endif

std::string MapView::createDatabaseSpawn
  (bool creature, std::uint32_t entry, glm::vec3 const& position, bool interactive)
{
#ifdef USE_MYSQL_UID_STORAGE
  auto const report = [this, interactive] (std::string const& message, bool is_error)
  {
    return reportDatabaseSpawnOutcome(message, is_error, interactive);
  };

  if (!Noggit::Database::DatabaseSettings::isEnabled())
  {
    return report("The database feature is not enabled. Turn it on and set the connection "
                  "details in Settings first.", true);
  }

  if (entry == 0)
  {
    return report("Enter the creature_template or gameobject_template entry id to place. 0 names"
                  " no template row.", true);
  }

  try
  {
    // READ_ONLY, like every other query this class issues. Creating a spawn writes nothing to any
    // database -- it adds a pending row to the scene cache, and only the changeset carries it out
    // of the editor.
    auto const config (Noggit::Database::DatabaseSettings::readConnectionConfig());

    Noggit::Database::WorldDatabaseConnection connection
      ( config
      , Noggit::Database::AccessMode::READ_ONLY
      , Noggit::Database::DatabaseSettings::readWritableSchema()
      );

    // Cached, NOT re-read. This runs from mousePressEvent, on the GUI thread, once per click in
    // place mode -- and re-reading information_schema.columns for the whole schema there stalled
    // the editor on every attempt to put a creature down. The two template lookups below are what
    // this connection is actually for: they are indexed single-row SELECTs on an entry id.
    Noggit::Database::SchemaModel const& schema (databaseSchemaFor(connection, config));

    // Through the tested seam, never by hand. positionFor / serverPositionFor are exact inverses
    // and this is the direction the editor is unusual for taking: everything else reads a server
    // coordinate and displays it, while this takes a place on screen and has to name it in the
    // frame the row will be written in.
    Noggit::Database::NoggitPlacement placement;
    placement.x = position.x;
    placement.y = position.y;
    placement.z = position.z;

    Noggit::Database::WorldPosition const world
      (Noggit::Database::SpawnPlacement::serverPositionFor(placement));

    if (!_db_spawn_scene)
    {
      _db_spawn_scene = std::make_unique<Noggit::Database::SpawnSceneCache>(getRenderContext());
    }

    // Building the entry constructs a ModelInstance and takes a model reference, and a refused
    // create releases one again on the way out. Both need a context bound -- see the long note in
    // loadDatabaseSpawnsForTiles, and the warning on SpawnSceneCache::addCreature.
    makeCurrent();
    OpenGL::context::scoped_setter const _gl_context (::gl, context());

    Noggit::Database::SpawnCreation created;
    std::string label;

    if (creature)
    {
      Noggit::Database::CreatureSpawn spawn;
      spawn.id = entry;
      spawn.map = static_cast<std::uint16_t>(_world->getMapID());
      spawn.position = world;

      // Facing north, deterministically, rather than derived from wherever the camera happens to
      // be pointing. A placement that came out at a different angle depending on the approach
      // would be impossible to repeat, and the facing is one spin box away.
      spawn.orientation = 0.0;

      // The core's defaults for a spawn a GM would create: idle, no wander, alive.
      // MovementType 0 with wander_distance 0 is the pairing the core requires, and
      // SpawnValidation enforces it on the way into the changeset.
      spawn.movement_type = Noggit::Database::MovementType::IDLE;
      spawn.wander_distance = 0.0;
      spawn.cur_health = 1;

      spawn.template_info = creatureTemplateFor(connection, schema, entry);
      label = spawn.template_info.name;

      created = _db_spawn_scene->addCreature(spawn);
    }
    else
    {
      Noggit::Database::GameObjectSpawn spawn;
      spawn.id = entry;
      spawn.map = static_cast<std::uint16_t>(_world->getMapID());
      spawn.position = world;
      spawn.orientation = 0.0;

      // Left at the identity so the emitter derives it from `orientation`. Writing a quaternion
      // here as well would be two statements of the same fact, and the emitter warns when they
      // disagree precisely because they are so easy to let drift.
      spawn.template_info = gameObjectTemplateFor(connection, schema, entry);
      label = spawn.template_info.name;

      created = _db_spawn_scene->addGameObject(spawn);
    }

    if (!created.created())
    {
      return report
        ( std::string("Entry ") + std::to_string(entry) + " cannot be placed: "
        + created.failure_reason
        + "\n\nNothing was created. A spawn with no model cannot be selected, moved or seen, so"
          " it would exist only as a row in the changeset."
        , true
        );
    }

    // Turned on for the same reason the load path turns it on: having placed something and then
    // not shown it reads as the placement having failed.
    _draw_db_spawns.set(true);
    _db_spawn_scene->setSelected(created.spawn);

    if (_db_spawn_panel)
    {
      _db_spawn_panel->refresh();
      _db_spawn_panel->selectSpawn(created.spawn);
    }

    markSpawnOverlayDirty();

    std::string const message
      ( std::string("Placed ") + (creature ? "creature" : "gameobject") + " entry "
      + std::to_string(entry) + (label.empty() ? std::string() : " (" + label + ")")
      + ". Its guid is allocated from MAX(guid) when the changeset is applied; nothing is written"
        " until you save." );

    Log << message << std::endl;
    _main_window->statusBar()->showMessage(QString::fromStdString(message), 5000);

    // Not through `report`: success on this path must not raise a modal, because placing spawns
    // is something a user does repeatedly and a dialog per placement is unusable.
    return "OK " + message;
  }
  catch (std::exception const& e)
  {
    LogError << "Creating a database spawn failed: " << e.what() << std::endl;

    return report(std::string("Could not place the spawn. ") + e.what(), true);
  }
#else
  (void)creature;
  (void)entry;
  (void)position;

  if (interactive)
  {
    QMessageBox::information
      ( this
      , "Database spawns"
      , "This build has no database support. Reconfigure with -DUSE_SQL=ON."
      );
  }

  return "ERR this build has no database support (reconfigure with -DUSE_SQL=ON)";
#endif
}

std::string MapView::deleteDatabaseSpawn
  (Noggit::Database::SpawnRef const& spawn, bool interactive)
{
  auto const report = [this, interactive] (std::string const& message, bool is_error)
  {
    return reportDatabaseSpawnOutcome(message, is_error, interactive);
  };

  if (!_db_spawn_scene || !spawn.valid())
  {
    return report("Select a spawn in the list first.", true);
  }

  // Counted before and after rather than compared against the provisional guid range, which
  // would put a second copy of PROVISIONAL_GUID_BASE here to fall out of step with the one in
  // SpawnSceneCache. removedSpawns() is the cache's own answer to "will this produce a DELETE",
  // and it is the same answer saveDatabaseChanges will act on.
  std::size_t const tombstones_before (_db_spawn_scene->removedSpawns().size());

  // Deleting a spawn the user PLACED destroys it outright -- it has no row to delete and nothing
  // to be restored to, so leaving it in the cache's tombstone list only made it unreachable. That
  // destruction releases its model reference, and the last reference to a Model destroys OpenGL
  // vertex arrays from a destructor that throws when no context is bound: a terminate, not an
  // error. Deleting a database-issued spawn still releases nothing, but this path cannot know
  // which it was handed until remove() has run, so the guard covers both.
  //
  // Same guard, same reason, as loadDatabaseSpawnsForTiles and discardDatabaseSpawnChanges.
  makeCurrent();
  OpenGL::context::scoped_setter const _gl_context (::gl, context());

  if (!_db_spawn_scene->remove(spawn))
  {
    return report("That spawn is not loaded, so there is nothing to delete.", true);
  }

  // No new tombstone means remove() destroyed it, which it does only for a spawn the user placed.
  // Still derived from the cache's own answer rather than from the guid, so the two cannot drift.
  bool const was_new (_db_spawn_scene->removedSpawns().size() == tombstones_before);

  if (_db_spawn_panel)
  {
    _db_spawn_panel->refresh();
  }

  markSpawnOverlayDirty();

  // The two cases are genuinely different and the user is entitled to know which one happened.
  // Deleting a spawn the user placed a moment ago produces no SQL at all; deleting one the
  // database issued produces a DELETE that a reviewer will see.
  std::string const message
    ( was_new
        ? std::string("Removed the spawn you placed. It was never in the database, so nothing is"
                      " deleted and no SQL is emitted for it.")
        : std::string("Marked ")
            + (spawn.kind == Noggit::Database::SpawnKind::CREATURE ? "creature" : "gameobject")
            + " guid " + std::to_string(spawn.guid)
            + " for deletion. The DELETE is written when you save; Discard puts it back."
    );

  Log << message << std::endl;
  _main_window->statusBar()->showMessage(QString::fromStdString(message), 5000);

  return "OK " + message;
}

std::string MapView::discardDatabaseSpawnChanges()
{
  if (!_db_spawn_scene)
  {
    return "OK Nothing to discard.";
  }

  std::size_t const pending (_db_spawn_scene->pendingCount());

  // Dropping a created spawn releases the last reference to its Model, which destroys OpenGL
  // vertex arrays from a destructor that throws when no context is bound -- a terminate, not an
  // error. Same guard, same reason, as loadDatabaseSpawnsForTiles.
  makeCurrent();
  OpenGL::context::scoped_setter const _gl_context (::gl, context());

  std::size_t const dropped (_db_spawn_scene->discardPending());

  if (_db_spawn_panel)
  {
    _db_spawn_panel->refresh();
  }

  markSpawnOverlayDirty();

  std::string message
    (std::to_string(pending) + " unsaved change(s) discarded");

  if (dropped > 0)
  {
    message += ", including " + std::to_string(dropped) + " placed spawn(s) removed";
  }

  message += ". Deleted spawns are back where they were.";

  return "OK " + message;
}

std::string MapView::saveDatabaseChanges(bool apply_to_dev, bool interactive)
{
#ifdef USE_MYSQL_UID_STORAGE
  auto const report = [this, interactive] (std::string const& message, bool is_error)
  {
    if (interactive)
    {
      if (is_error)
      {
        QMessageBox::critical(this, "Database spawns", QString::fromStdString(message));
      }
      else
      {
        QMessageBox::information(this, "Database spawns", QString::fromStdString(message));
      }
    }

    return (is_error ? std::string("ERR ") : std::string("OK ")) + message;
  };

  if (!_db_spawn_scene || _db_spawn_scene->pendingCount() == 0)
  {
    // pendingCount, not dirtyCount: a session that only placed or only deleted spawns has moved
    // nothing, and refusing to save it would silently discard the whole of the user's work.
    return report("Nothing has been changed, so there is nothing to save.", true);
  }

  try
  {
    auto const read_config (Noggit::Database::DatabaseSettings::readConnectionConfig());

    Noggit::Database::WorldDatabaseConnection read_connection
      ( read_config
      , Noggit::Database::AccessMode::READ_ONLY
      , Noggit::Database::DatabaseSettings::readWritableSchema()
      );

    // Cached like the create path. ChangesetBuilder takes its SchemaModel by value, so what the
    // emitter reasons about is a copy taken here -- a later settings change cannot retune a
    // changeset already being built.
    Noggit::Database::SchemaModel const& schema (databaseSchemaFor(read_connection, read_config));

    Noggit::Database::ChangesetBuilder::Options options;
    options.description = "Spawn edits from Noggit: moves, placements and deletions";

    Noggit::Database::ChangesetBuilder builder (schema, options);

    // Three lists, three emitter entry points, and the separation matters. dirtyEntries() is
    // edits only -- a spawn the user placed is excluded there even after being dragged, because
    // its guid is provisional and an INSERT keyed off it would overwrite a real row. See
    // SpawnSceneEntry::is_new.
    auto const dirty (_db_spawn_scene->dirtyEntries());
    auto const created (_db_spawn_scene->newEntries());
    auto const removed (_db_spawn_scene->removedSpawns());

    for (auto const* entry : dirty)
    {
      if (entry->kind == Noggit::Database::SpawnKind::CREATURE)
      {
        builder.addCreature(entry->creature);
      }
      else
      {
        builder.addGameObject(entry->gameobject);
      }
    }

    for (auto const* entry : created)
    {
      if (entry->kind == Noggit::Database::SpawnKind::CREATURE)
      {
        builder.addNewCreature(entry->creature);
      }
      else
      {
        builder.addNewGameObject(entry->gameobject);
      }
    }

    // removedSpawns() carries only guids the database issued: a spawn created and deleted again
    // before saving is absent from all three lists and produces no SQL whatever.
    for (auto const& spawn : removed)
    {
      if (spawn.kind == Noggit::Database::SpawnKind::CREATURE)
      {
        // Takes the creature_addon row with it. That is the one case where clearing the addon is
        // right -- the creature row it belonged to is going away, so what would be left is an
        // orphan rather than anybody's data.
        builder.removeCreature(spawn.guid);
      }
      else
      {
        builder.removeGameObject(spawn.guid);
      }
    }

    // Throws when a spawn would be rejected by the core's own load-time validation, which is the
    // point: a changeset that MySQL accepts and the server then silently corrects is worse than
    // one that was never written.
    std::string const sql (builder.build());

    // Beside the project, not beside the binary: this is project data, and it is what the user
    // will hand to whoever applies it.
    QString directory
      (QString::fromStdString(Noggit::Project::CurrentProject::get()->ProjectPath));

    if (!(directory.endsWith('/') || directory.endsWith('\\')))
    {
      directory += "/";
    }

    directory += "changesets/";
    QDir().mkpath(directory);

    QString const path
      ( directory + "spawns_"
      + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".sql");

    QFile file (path);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
      return report("Could not write " + path.toStdString(), true);
    }

    file.write(sql.data(), static_cast<qint64>(sql.size()));
    file.close();

    std::string message
      ( std::to_string(dirty.size()) + " moved, " + std::to_string(created.size())
      + " placed and " + std::to_string(removed.size()) + " deleted spawn(s) written to "
      + path.toStdString() );

    if (apply_to_dev)
    {
      // DEV_WRITE refuses to construct unless the target schema is exactly the configured
      // writable one, so this cannot reach a live schema even if the settings point at one.
      auto config (Noggit::Database::DatabaseSettings::readConnectionConfig());
      config.schema = Noggit::Database::DatabaseSettings::readWritableSchema();

      // readWritableSchema() again, NOT config.schema. Passing config.schema would compare the
      // value against the line that just assigned it -- a guard that cannot fail, leaving the
      // whole protection resting on the assignment above rather than on the check. The other two
      // call sites in this file already pass it directly; this one did not.
      Noggit::Database::WorldDatabaseConnection write_connection
        ( config
        , Noggit::Database::AccessMode::DEV_WRITE
        , Noggit::Database::DatabaseSettings::readWritableSchema()
        );

      std::size_t const statements (write_connection.executeScript(sql));

      message += ", and applied to " + config.schema
               + " (" + std::to_string(statements) + " statements)";
    }

    // Dropping the placed spawns releases their model references, which destroys OpenGL vertex
    // arrays from a destructor -- the same terminate-rather-than-throw hazard the load path
    // guards. This slot had no context bound before creation existed, because clearDirty()
    // destroyed nothing.
    makeCurrent();
    OpenGL::context::scoped_setter const _gl_context (::gl, context());

    std::size_t const dropped (_db_spawn_scene->clearPending());

    if (dropped > 0)
    {
      // Said plainly, because the spawns visibly disappear and the reason is not guessable. Their
      // guids are still the editor's invention: the file has been written, not necessarily
      // applied, and even when it has been, nothing told the editor which guids the server chose.
      // Keeping them would leave entries whose next save either creates them a second time or
      // writes a fictional guid into an INSERT that overwrites a real row.
      message += ". The " + std::to_string(dropped) + " placed spawn(s) have been removed from the"
                 " view: their guids are chosen by the server when the file is applied, so reload"
                 " the tile to see them with the guids the database issued";
    }

    // Refreshed here rather than only in the panel's own handler: clearPending() drops the placed
    // spawns from the cache, so a save driven from the menu would otherwise leave the list
    // showing entries that no longer exist.
    if (_db_spawn_panel)
    {
      _db_spawn_panel->refresh();
    }

    markSpawnOverlayDirty();

    Log << message << std::endl;
    _main_window->statusBar()->showMessage(QString::fromStdString(message), 8000);

    return "OK " + message;
  }
  catch (std::exception const& e)
  {
    LogError << "Saving database spawn changes failed: " << e.what() << std::endl;

    return report(std::string("Could not save changes. ") + e.what(), true);
  }
#else
  (void)apply_to_dev;
  (void)interactive;

  return "ERR this build has no database support (reconfigure with -DUSE_SQL=ON)";
#endif
}

std::string MapView::selectedTexturePath() const
{
  auto const index = static_cast<std::size_t>(editing_mode::paint);

  if (index >= _tools.size() || !_tools[index])
  {
    return {};
  }

  auto const* texturing = dynamic_cast<Noggit::TexturingTool const*>(_tools[index].get());

  return texturing ? texturing->selectedTexturePath() : std::string();
}

glm::vec3 MapView::cameraPosition() const
{
  return _camera.position;
}

Noggit::Database::SpawnSceneCache* MapView::databaseSpawns() const
{
  return _db_spawn_scene.get();
}

void MapView::markSpawnOverlayDirty()
{
  _needs_redraw = true;
}

Noggit::Database::SpawnRef MapView::pickDatabaseSpawn()
{
  if (!_db_spawn_scene)
  {
    // Default-constructed: guid 0, which SpawnRef::valid() reports as nothing selected.
    return {};
  }

  math::ray const ray (intersect_ray());

  Noggit::Database::SpawnRef nearest;
  float nearest_distance = std::numeric_limits<float>::max();

  for (auto const* entry : _db_spawn_scene->allEntries())
  {
    if (!entry->instance)
    {
      continue;
    }

    // ModelInstance::intersect returns early on a model that has not finished loading, so a
    // half-streamed tile picks nothing rather than picking wrongly.
    selection_result hits;

    entry->instance->intersect
      (_model_view, ray, &hits, static_cast<int>(_world->animtime), _draw_model_animations.get());

    for (auto const& hit : hits)
    {
      if (hit.first < nearest_distance)
      {
        nearest_distance = hit.first;
        nearest = entry->ref();
      }
    }
  }

  return nearest;
}

bool MapView::focusOnSpawn(Noggit::Database::SpawnRef const& spawn, float distance)
{
  glm::vec3 target (0.0f, 0.0f, 0.0f);

  if (!_db_spawn_scene || !_db_spawn_scene->positionOf(spawn, target))
  {
    return false;
  }

  // The tile is already loaded -- a spawn cannot be in the scene cache otherwise -- so the
  // force-load focusOnPoint offers is skipped here rather than paying for a hasTile probe.
  if (!focusOnPoint(target, distance, false))
  {
    return false;
  }

  _db_spawn_scene->setSelected(spawn);

  return true;
}

bool MapView::focusOnPoint(glm::vec3 const& target, float distance, bool load_tile)
{
  // Copied from move_camera_with_auto_height (MapView.cpp:4429-4432), including the
  // wait_until_loaded: a row in the Missing Objects panel outlives the ADT it came from, and
  // flying to a coordinate in an unloaded tile otherwise shows the mapper empty space and no
  // placeholder, which reads as "the tool is lying about this row".
  if (load_tile)
  {
    TileIndex const tile_index (target);

    if (!tile_index.is_valid())
    {
      return false;
    }

    if (_world->mapIndex.hasTile(tile_index))
    {
      makeCurrent();
      OpenGL::context::scoped_setter const _ (::gl, context());

      _world->mapIndex.loadTile(target)->wait_until_loaded();
    }
  }

  // Stand back and above, then solve the angles rather than guessing them. Camera::direction is
  // (sin(yaw)cos(pitch), -sin(pitch), cos(yaw)cos(pitch)), so a look vector d gives
  // yaw = atan2(d.x, d.z) and pitch = asin(-d.y / |d|).
  glm::vec3 const eye (target.x, target.y + distance * 0.45f, target.z + distance);
  glm::vec3 const to_target (target - eye);
  float const length (glm::length(to_target));

  if (length < 1e-4f)
  {
    return false;
  }

  _camera.position = eye;
  _camera.yaw(math::degrees(glm::degrees(std::atan2(to_target.x, to_target.z))));
  _camera.pitch(math::degrees(glm::degrees(std::asin(-to_target.y / length))));

  _camera_moved_since_last_draw = true;
  _needs_redraw = true;

  return true;
}

bool MapView::objectGizmoHasTarget() const
{
  return Noggit::Ui::Tools::ViewportGizmo::ViewportGizmo::drawsForSelection
    (_world->current_selection());
}

bool MapView::gizmoIsDrawn() const
{
  if (!_gizmo_on.get())
  {
    return false;
  }

  // The three branches of the gizmo block in paintGL, in the same order and on the same
  // conditions. Each one already yields to the ones above it -- spawnGizmoTarget() returns nothing
  // while the object gizmo has a target, lightGizmoTarget() returns 0 while either of the other
  // two does -- so this is not "any of them could draw", it is "exactly one of them does".
  //
  // The light term matters as much as the others: without it, every "the gizmo gets this click"
  // guard would let a click on a light's translate arrow fall through to the light pick underneath
  // it, which re-picks whatever sphere is behind the handle and moves a different light than the
  // one that was grabbed.
  return objectGizmoHasTarget() || spawnGizmoTarget().valid() || lightGizmoTarget() != 0;
}

Noggit::Database::SpawnRef MapView::spawnGizmoTarget() const
{
  // Same three preconditions the shift-click pick uses (MapView::mousePressEvent), so the gizmo
  // cannot appear for someone who has the overlay off or the panel closed.
  if (!_db_spawn_scene || !_db_spawn_panel || !_draw_db_spawns.get())
  {
    return {};
  }

  // The object editor wins when it has a target. Only one gizmo is ever drawn, which is what keeps
  // the two from fighting over a drag -- see the note on this declaration.
  //
  // Not has_selection(): draw_map auto-selects the chunk under the cursor every frame the selection
  // is empty, so has_selection() is permanently true after one mouse movement over terrain and this
  // returned nothing except when the camera was pointed at the sky. A chunk is not a competing
  // gizmo target, and objectGizmoHasTarget() is the object gizmo's own answer to that.
  if (objectGizmoHasTarget())
  {
    return {};
  }

  Noggit::Database::SpawnRef const selected (_db_spawn_scene->selected());

  if (!selected.valid())
  {
    return {};
  }

  // Loaded-ness is checked here rather than trusted, because setSelected takes any ref and a
  // reload empties the cache without clearing it. positionOf is the cheapest question that
  // answers "is this still there", and it is the same value the gizmo is about to be drawn at.
  glm::vec3 position (0.0f, 0.0f, 0.0f);

  return _db_spawn_scene->positionOf(selected, position) ? selected : Noggit::Database::SpawnRef{};
}

void MapView::handleSpawnGizmo(Noggit::Database::SpawnRef const& spawn)
{
  glm::vec3 position (0.0f, 0.0f, 0.0f);

  if (!_db_spawn_scene || !_db_spawn_scene->positionOf(spawn, position))
  {
    return;
  }

  _transform_gizmo.setCurrentGizmoOperation(_gizmo_operation);
  _transform_gizmo.setCurrentGizmoMode(_gizmo_mode);

  auto const delta
    (_transform_gizmo.handleDetachedGizmo(position, _model_view, _projection));

  if (!delta.active)
  {
    return;
  }

  _spawn_gizmo_dragging = true;

  bool changed = false;

  if (_gizmo_operation == ImGuizmo::ROTATE)
  {
    if (delta.yaw_degrees != 0.0f)
    {
      // dir.y = degrees(orientation) + YAW_OFFSET_DEGREES (SpawnPlacement.hpp:84), so the offset
      // cancels in a difference and a Noggit yaw delta is a server orientation delta of the same
      // sign and magnitude. Only the delta is derived here; every absolute conversion still goes
      // through SpawnPlacement, inside rotateTo.
      double current = 0.0;

      if (_db_spawn_scene->orientationOf(spawn, current))
      {
        // rotateTo, never the instance's dir directly: for a gameobject it also rewrites
        // rotation0..3, and the core reads `orientation` while the client renders the quaternion.
        // Writing one without the other produces a spawn that faces differently in game than here.
        changed = _db_spawn_scene->rotateTo
          (spawn, current + glm::radians(static_cast<double>(delta.yaw_degrees)));
      }
    }
  }
  else
  {
    if (delta.translation.x != 0.0f || delta.translation.y != 0.0f || delta.translation.z != 0.0f)
    {
      // The gizmo reports a delta; moveTo takes an absolute. Added to the position just read back
      // rather than to a running total kept here, so nothing accumulates rounding across a drag.
      // moveTo converts to server coordinates through SpawnPlacement::serverPositionFor and marks
      // the entry dirty, which is what puts it in the changeset.
      changed = _db_spawn_scene->moveTo(spawn, position + delta.translation);
    }
  }

  if (changed)
  {
    // Not _db_spawn_panel->refresh() here: that rebuilds the list widget, and doing it every frame
    // of a drag churns the selection. It happens once, on release, in mouseReleaseEvent.
    _needs_redraw = true;
  }
}

void MapView::setLightEditor(Noggit::Ui::Tools::LightEditor* editor)
{
  _light_editor = editor;
}

int MapView::pickLight() const
{
  auto& skies = _world->renderer()->skies();

  if (!skies)
  {
    return 0;
  }

  math::ray const ray (intersect_ray());

  // math::ray keeps its origin and direction private and exposes no accessor, so both are
  // recovered from the one thing it does offer. position(t) is defined as origin + direction * t,
  // which makes position(0) the origin exactly and position(1) - position(0) the direction
  // exactly. Recovered rather than rebuilt from _camera.position, because intersect_ray is also
  // what the 2D display mode uses and there the ray does not start at the camera at all.
  glm::vec3 const origin (ray.position(0.0f));
  glm::vec3 const raw_direction (ray.position(1.0f) - origin);
  float const direction_length (glm::length(raw_direction));

  if (direction_length < 1e-6f)
  {
    return 0;
  }

  glm::vec3 const direction (raw_direction / direction_length);

  // math::ray::intersects_sphere is deliberately not used here, for two independent reasons.
  // It assumes a unit direction -- intersect_ray builds `far_plane_point - camera`, a vector as
  // long as the view distance -- and it returns NO HIT whenever the origin is inside the sphere
  // (math/ray.cpp: `if (p_d > 0 || dot(p, p) < rSquared) return {..., false};`). Outer light radii
  // in 3.3.5 run past 3000 yards and the camera is inside several of them at once, so that early
  // return alone would make most lights unclickable.

  // A clickable handle around the light's centre, sized to a constant number of PIXELS.
  //
  // A sphere of radius R at distance d has a screen diameter of
  // (2R / d) / (2 * tan(fov / 2)) * viewport_height pixels. Solving for R at a target diameter
  // gives R = d * tan(fov / 2) * target / height. With this project's default 54 degree vertical
  // FOV (Camera.cpp) and a 1080 pixel viewport that is R = d * tan(27 deg) * 18 / 1080
  // = d * 0.008492, i.e. 8.5 yards at a kilometre -- and 18 pixels on screen wherever the light
  // sits. Without it a light with a 40 yard outer radius is a sub-pixel target from any distance.
  float constexpr HANDLE_DIAMETER_PIXELS = 18.0f;
  float constexpr MIN_HANDLE_RADIUS = 2.0f;

  float handle_scale = 0.0f;

  if (_display_mode == display_mode::in_3D)
  {
    handle_scale = std::tan(_camera.fov()._ * 0.5f) * HANDLE_DIAMETER_PIXELS
                 / std::max(1.0f, static_cast<float>(height()));
  }

  struct Candidate
  {
    float radius;
    float centre_distance;
    int id;
  };

  std::vector<Candidate> candidates;

  for (Sky const& sky : skies->skies)
  {
    // The global light is defined by sitting at 0,0,0 and WorldRender draws no sphere for it
    // (rendering/WorldRender.cpp skips sky.global in both sphere passes), so there is nothing on
    // screen to have clicked.
    if (sky.global)
    {
      continue;
    }

    glm::vec3 const to_centre (sky.pos - origin);
    float const centre_distance (glm::length(to_centre));
    float const handle_radius (std::max(MIN_HANDLE_RADIUS, centre_distance * handle_scale));
    float const pick_radius (std::max(sky.r2, handle_radius));
    float const pick_radius_squared (pick_radius * pick_radius);

    // Ray/sphere by closest approach.
    float const along (glm::dot(to_centre, direction));
    float const perpendicular_squared (glm::dot(to_centre, to_centre) - along * along);

    if (perpendicular_squared > pick_radius_squared)
    {
      continue;
    }

    float const half_chord (std::sqrt(std::max(0.0f, pick_radius_squared - perpendicular_squared)));

    // The FAR intersection, not the near one. `along + half_chord >= 0` accepts a sphere the
    // camera is standing inside, which for a light is the ordinary case rather than the exception.
    if (along + half_chord < 0.0f)
    {
      continue;
    }

    candidates.push_back({sky.r2, centre_distance, sky.getId()});
  }

  if (candidates.empty())
  {
    return 0;
  }

  // THE OVERLAP RULE: smallest outer radius first, then nearest centre, then lowest id -- and a
  // repeat click on the same spot steps to the next candidate, wrapping.
  //
  // Smallest-first because that is the engine's own precedence, not a guess. Sky::operator< sorts
  // the light vector by ascending r2 with the global last, with the comment "smaller skies will
  // have precedence", and Skies::findSkyWeights gives a light weight 1.0 anywhere inside its inner
  // radius. So of two nested lights the smaller one is the one actually colouring the sky at the
  // point clicked, and selecting it is what "I clicked that light" means. Nearest-surface would
  // instead select whichever enormous zone light happened to have its shell closer to the eye.
  //
  // The cycle is what keeps the enclosing lights reachable at all: click once for the light that
  // governs the spot, click again without moving the cursor to walk outward through the ones
  // containing it, and wrap round. Moving the cursor changes the candidate set, the current
  // selection is then usually not in it, and the rule resets to "smallest".
  std::sort(candidates.begin(), candidates.end()
    , [] (Candidate const& a, Candidate const& b)
      {
        if (a.radius != b.radius)
        {
          return a.radius < b.radius;
        }

        if (a.centre_distance != b.centre_distance)
        {
          return a.centre_distance < b.centre_distance;
        }

        return a.id < b.id;
      });

  int const currently_selected (skies->selectedLight());

  for (std::size_t i = 0; i < candidates.size(); ++i)
  {
    if (candidates[i].id == currently_selected)
    {
      return candidates[(i + 1) % candidates.size()].id;
    }
  }

  return candidates.front().id;
}

void MapView::selectLight(int light_id)
{
  auto& skies = _world->renderer()->skies();

  if (!skies)
  {
    return;
  }

  skies->setSelectedLight(light_id);

  if (_light_editor)
  {
    _light_editor->onLightSelectedInViewport(light_id);
  }

  _needs_redraw = true;
}

bool MapView::scaleSelectedLightRadii(float factor)
{
  auto& skies = _world->renderer()->skies();

  if (!skies || !std::isfinite(factor) || factor <= 0.0f)
  {
    return false;
  }

  Sky* const sky = skies->findSkyById(skies->selectedLight());

  if (!sky || sky->global)
  {
    return false;
  }

  // Clamped to the same 0..100000 range the panel's radius spin boxes accept, so a long scroll
  // cannot leave the light holding a number the panel would silently refuse to show.
  float constexpr MAX_LIGHT_RADIUS = 100000.0f;

  sky->r1 = std::min(MAX_LIGHT_RADIUS, std::max(0.0f, sky->r1 * factor));
  sky->r2 = std::min(MAX_LIGHT_RADIUS, std::max(0.0f, sky->r2 * factor));

  // Weights are cached until something invalidates them, so without this the sphere changes size
  // and the sky colour keeps blending as though it had not.
  skies->force_update();

  if (_light_editor)
  {
    _light_editor->refreshSelectedLightFields();
  }

  _needs_redraw = true;

  return true;
}

int MapView::lightGizmoTarget() const
{
  // Light mode only. Otherwise the gizmo would sit over the terrain brushes in every other mode,
  // claiming clicks for a light the user is not editing.
  if (terrainMode != editing_mode::light)
  {
    return 0;
  }

  auto& skies = _world->renderer()->skies();

  if (!skies)
  {
    return 0;
  }

  // Same precedence chain spawnGizmoTarget joined, one step further down: object editor first,
  // then database spawn, then light. All three share one ImGuizmo context and IsUsing() ignores
  // the gizmo id, so two drawn in a frame would both respond to the same drag.
  //
  // Not has_selection() for the reason spawnGizmoTarget spells out: draw_map auto-selects the
  // chunk under the cursor on every frame the selection is empty, so has_selection() is
  // permanently true after one mouse movement over terrain.
  if (objectGizmoHasTarget() || spawnGizmoTarget().valid())
  {
    return 0;
  }

  int const selected (skies->selectedLight());

  if (!selected)
  {
    return 0;
  }

  // Re-resolved rather than trusted: setSelectedLight accepts any id, a map load rebuilds Skies
  // from scratch, and deleteSky can remove the row underneath it. findSkyById is also the exact
  // question handleLightGizmo is about to ask for the position.
  Sky const* const sky = skies->findSkyById(selected);

  // A global light is global because it sits at 0,0,0. Dragging one would stop it being global.
  return (sky && !sky->global) ? selected : 0;
}

void MapView::handleLightGizmo(int light_id)
{
  auto& skies = _world->renderer()->skies();

  if (!skies)
  {
    return;
  }

  Sky* const sky = skies->findSkyById(light_id);

  if (!sky)
  {
    return;
  }

  glm::vec3 const position (sky->pos);

  // TRANSLATE regardless of what the shared toolbar is set to, restored immediately afterwards.
  //
  // A light is a point with two radii and no orientation of any kind, so a rotate ring would turn
  // and change nothing: handleDetachedGizmo would hand back a yaw delta with nowhere to put it.
  // It already substitutes TRANSLATE for SCALE and BOUNDS for exactly this reason, and ROTATE is
  // the one case it cannot substitute on its own, because a database spawn genuinely does have a
  // facing. Radii are edited in the panel and with ctrl+wheel -- see scaleSelectedLightRadii.
  _transform_gizmo.setCurrentGizmoOperation(ImGuizmo::TRANSLATE);
  _transform_gizmo.setCurrentGizmoMode(ImGuizmo::MODE::WORLD);

  auto const delta (_transform_gizmo.handleDetachedGizmo(position, _model_view, _projection));

  // Put back, because the object and spawn paths read the operation and mode off the shared gizmo
  // object rather than setting them unconditionally every frame.
  _transform_gizmo.setCurrentGizmoOperation(_gizmo_operation);
  _transform_gizmo.setCurrentGizmoMode(_gizmo_mode);

  if (!delta.active)
  {
    return;
  }

  _light_gizmo_dragging = true;

  if (delta.translation.x == 0.0f && delta.translation.y == 0.0f && delta.translation.z == 0.0f)
  {
    return;
  }

  // Added to the position read back this frame rather than to a running total kept here, so
  // nothing accumulates rounding across a drag. Same rule as handleSpawnGizmo.
  sky->pos = position + delta.translation;

  // 0,0,0 is how a map's single global light is spelled -- Sky's constructor derives `global` from
  // exactly that comparison. Dragging a light onto the origin would silently promote it and demote
  // the real global light in the same frame, so the origin is stepped over rather than accepted.
  if (sky->pos.x == 0.0f && sky->pos.y == 0.0f && sky->pos.z == 0.0f)
  {
    sky->pos.x = 0.01f;
  }

  // Weights are cached until invalidated; without this the light moves and the sky colour does not
  // follow until the camera happens to move too.
  skies->force_update();

  _needs_redraw = true;
}

std::map<std::string, std::size_t> MapView::terrainTexturesInScope(bool all_loaded_tiles) const
{
  std::map<std::string, std::size_t> layers_by_texture;

  auto const scan_tile = [&layers_by_texture] (MapTile* tile)
  {
    if (!tile)
    {
      return;
    }

    for (int z = 0; z < 16; ++z)
    {
      for (int x = 0; x < 16; ++x)
      {
        MapChunk* chunk = tile->getChunk(static_cast<unsigned>(x), static_cast<unsigned>(z));

        if (!chunk || !chunk->texture_set)
        {
          continue;
        }

        for (std::size_t layer = 0; layer < chunk->texture_set->num(); ++layer)
        {
          ++layers_by_texture[chunk->texture_set->filename(layer)];
        }
      }
    }
  };

  if (all_loaded_tiles)
  {
    for (MapTile* tile : _world->mapIndex.loaded_tiles())
    {
      scan_tile(tile);
    }
  }
  else
  {
    scan_tile(_world->mapIndex.getTile(::TileIndex(_camera.position)));
  }

  return layers_by_texture;
}

void MapView::enterEvent(QEvent* event)
{
  // check if noggit is the currently active windows
  if (static_cast<QApplication*>(QApplication::instance())->applicationState() & Qt::ApplicationActive)
  {
    activateWindow();
  }
}

void MapView::setupViewportOverlay()
{
  _overlay_widget = new QWidget(this);
  _viewport_overlay_ui = new ::Ui::MapViewOverlay();
  _viewport_overlay_ui->setupUi(_overlay_widget);
  _overlay_widget->setAttribute(Qt::WA_TranslucentBackground);
  _overlay_widget->setMouseTracking(true);
  _overlay_widget->setGeometry(0,0, width(), height());

  _viewport_overlay_ui->gizmoVisibleButton->setIcon(Noggit::Ui::FontNoggitIcon(Noggit::Ui::FontNoggit::Icons::GIZMO_VISIBILITY));
  _viewport_overlay_ui->gizmoModeButton->setIcon(Noggit::Ui::FontNoggitIcon(Noggit::Ui::FontNoggit::Icons::GIZMO_LOCAL));
  _viewport_overlay_ui->gizmoRotateButton->setIcon(Noggit::Ui::FontNoggitIcon(Noggit::Ui::FontNoggit::Icons::GIZMO_ROTATE));
  _viewport_overlay_ui->gizmoScaleButton->setIcon(Noggit::Ui::FontNoggitIcon(Noggit::Ui::FontNoggit::Icons::GIZMO_SCALE));
  _viewport_overlay_ui->gizmoTranslateButton->setIcon(Noggit::Ui::FontNoggitIcon(Noggit::Ui::FontNoggit::Icons::GIZMO_TRANSLATE));

  connect(this, &MapView::resized
    ,[this]()
          {
            _overlay_widget->setGeometry(0, 0, width(), height());
          }
  );

  connect(_viewport_overlay_ui->gizmoVisibleButton, &QPushButton::clicked
    ,[this]()
          {
            _gizmo_on.set(_viewport_overlay_ui->gizmoVisibleButton->isChecked());
          }
  );

  connect(&_gizmo_on, &Noggit::BoolToggleProperty::changed
    ,[this](bool state)
          {
            _viewport_overlay_ui->gizmoVisibleButton->setChecked(state);
          }
  );

  connect(_viewport_overlay_ui->gizmoModeButton, &QPushButton::clicked, [this]()
  {
      if (_viewport_overlay_ui->gizmoModeButton->isChecked())
      {
          _gizmo_mode = ImGuizmo::MODE::WORLD;
      }
      else
      {
          _gizmo_mode = ImGuizmo::MODE::LOCAL;
      }
  });

  connect(_viewport_overlay_ui->gizmoTranslateButton, &QPushButton::clicked, [this]() {
      updateGizmoOverlay(ImGuizmo::OPERATION::TRANSLATE);
    });

  connect(_viewport_overlay_ui->gizmoRotateButton, &QPushButton::clicked, [this]() {
      updateGizmoOverlay(ImGuizmo::OPERATION::ROTATE);
    });

  connect(_viewport_overlay_ui->gizmoScaleButton, &QPushButton::clicked, [this]() {
      updateGizmoOverlay(ImGuizmo::OPERATION::SCALE);
    });
}

void MapView::updateGizmoOverlay(ImGuizmo::OPERATION operation)
{
  if (operation == ImGuizmo::OPERATION::TRANSLATE)
  {
    _viewport_overlay_ui->gizmoRotateButton->setChecked(false);
    _viewport_overlay_ui->gizmoScaleButton->setChecked(false);

    if (!_viewport_overlay_ui->gizmoTranslateButton->isChecked())
      _viewport_overlay_ui->gizmoTranslateButton->setChecked(true);
  }

  if (operation == ImGuizmo::OPERATION::ROTATE)
  {
    _viewport_overlay_ui->gizmoTranslateButton->setChecked(false);
    _viewport_overlay_ui->gizmoScaleButton->setChecked(false);

    if (!_viewport_overlay_ui->gizmoRotateButton->isChecked())
      _viewport_overlay_ui->gizmoRotateButton->setChecked(true);
  }

  if (operation == ImGuizmo::OPERATION::SCALE)
  {
    _viewport_overlay_ui->gizmoTranslateButton->setChecked(false);
    _viewport_overlay_ui->gizmoRotateButton->setChecked(false);

    if (!_viewport_overlay_ui->gizmoScaleButton->isChecked())
      _viewport_overlay_ui->gizmoScaleButton->setChecked(true);
  }

  _gizmo_operation = operation;
}

void MapView::setupNodeEditor()
{
  auto _node_editor = new Noggit::Ui::Tools::NodeEditor::Ui::NodeEditorWidget(this);
  _node_editor_dock = new QDockWidget("Node editor", this);
  _node_editor_dock->setWidget(_node_editor);
  _node_editor_dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea | Qt::LeftDockWidgetArea);

  _main_window->addDockWidget(Qt::LeftDockWidgetArea, _node_editor_dock);
  _node_editor_dock->setFeatures(QDockWidget::DockWidgetMovable
                                 | QDockWidget::DockWidgetFloatable
                                 | QDockWidget::DockWidgetClosable);

  _node_editor_dock->setVisible(_settings->value ("map_view/node_editor", false).toBool());

  connect(_node_editor_dock, &QDockWidget::visibilityChanged,
          [=](bool visible)
          {
            if (ui_hidden)
              return;

            _settings->setValue ("map_view/node_editor", visible);
            _settings->sync();
          });

  connect(this, &QObject::destroyed, _node_editor_dock, &QObject::deleteLater);

  connect ( &_show_node_editor, &Noggit::BoolToggleProperty::changed
    , _node_editor_dock, [this]
            {
              if (!ui_hidden)
                _node_editor_dock->setVisible(_show_node_editor.get());
            }
  );

  connect ( _node_editor_dock, &QDockWidget::visibilityChanged
    , &_show_node_editor, &Noggit::BoolToggleProperty::set
  );

}

void MapView::setupAssetBrowser()
{
  _asset_browser_dock = new QDockWidget("Asset browser", this);
  _asset_browser = new Noggit::Ui::Tools::AssetBrowser::Ui::AssetBrowserWidget(this, this);

  //_main_window->addDockWidget(Qt::BottomDockWidgetArea, _asset_browser_dock);
  _asset_browser_dock->setFeatures(QDockWidget::DockWidgetMovable
                                   | QDockWidget::DockWidgetFloatable
                                   | QDockWidget::DockWidgetClosable);
  _asset_browser_dock->setAllowedAreas(Qt::NoDockWidgetArea);

  _asset_browser_dock->setFloating(true);
  _asset_browser_dock->hide();

  _asset_browser_dock->setWidget(_asset_browser);
  _asset_browser_dock->setWindowFlags(
    Qt::CustomizeWindowHint |
    Qt::Window | 
    Qt::WindowMinimizeButtonHint |
    Qt::WindowMaximizeButtonHint |
    Qt::WindowCloseButtonHint | 
    Qt::WindowStaysOnTopHint);

  connect(_asset_browser_dock, &QDockWidget::visibilityChanged,
          [=](bool visible)
          {
            if (ui_hidden)
              return;

            _settings->setValue ("map_view/asset_browser", visible);
            _settings->sync();
          });;

  connect(this, &QObject::destroyed, _asset_browser_dock, &QObject::deleteLater);
}

void MapView::setupDetailInfos()
{

  // Dock
  _detail_infos_dock = new QDockWidget("Detail info", this);
  _detail_infos_dock->setFeatures(QDockWidget::DockWidgetMovable
                                  | QDockWidget::DockWidgetFloatable
                                  | QDockWidget::DockWidgetClosable);

  _detail_infos_dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea | Qt::LeftDockWidgetArea);


  _main_window->addDockWidget(Qt::BottomDockWidgetArea, _detail_infos_dock);
  _detail_infos_dock->setFloating(true);
  _detail_infos_dock->hide();
  // End Dock

  guidetailInfos = new Noggit::Ui::detail_infos(this);
  _detail_infos_dock->setWidget(guidetailInfos);


  connect ( &_show_detail_info_window, &Noggit::BoolToggleProperty::changed
    , guidetailInfos, [this]
            {
              if (!ui_hidden)
              {
                  _detail_infos_dock->setVisible(_show_detail_info_window.get());
                  updateDetailInfos();
              }
            }
  );

  connect ( guidetailInfos, &Noggit::Ui::widget::visibilityChanged
    , &_show_detail_info_window, &Noggit::BoolToggleProperty::set
  );

  connect(NOGGIT_ACTION_MGR, &Noggit::ActionManager::onActionBegin,
    [this](Noggit::Action*)
    {
      updateDetailInfos();
    });

  connect(NOGGIT_ACTION_MGR, &Noggit::ActionManager::onActionEnd,
    [this](Noggit::Action*)
    {
      updateDetailInfos();
    });

  connect(NOGGIT_ACTION_MGR, &Noggit::ActionManager::currentActionChanged,
    [this](unsigned)
    {
      updateDetailInfos();
    });
}

void MapView::updateDetailInfos()
{
  auto& current_selection = _world->current_selection();

  // update detail infos TODO: selection update signal.


  if (guidetailInfos->isVisible())
  {
    if (!current_selection.empty())
    {
      selection_type& selection_last = const_cast<selection_type&>(current_selection.back());

      switch (selection_last.index())
      {
        case eEntry_Object:
        {
          auto obj = std::get<selected_object_type>(selection_last);
          obj->updateDetails(guidetailInfos);
          break;
        }
        case eEntry_MapChunk:
        {
          selected_chunk_type& chunk_sel(std::get<selected_chunk_type>(selection_last));
          chunk_sel.updateDetails(guidetailInfos);
          break;
        }
      }
    }
    else
    {
      guidetailInfos->setText("");
    }
  }
}

void MapView::setupToolbars()
{
  _toolbar = new Noggit::Ui::toolbar(_tools, [this] (editing_mode mode) { set_editing_mode (mode); });
  _toolbar->setOrientation(Qt::Vertical);
  auto left_toolbar_layout = new QVBoxLayout(_viewport_overlay_ui->leftToolbarHolder);
  left_toolbar_layout->addWidget( _toolbar);
  left_toolbar_layout->setDirection(QBoxLayout::LeftToRight);
  left_toolbar_layout->setContentsMargins(0, 5, 0, 5);
  connect (this, &QObject::destroyed, _toolbar, &QObject::deleteLater);

  auto left_sec_toolbar_layout = new QVBoxLayout(_viewport_overlay_ui->leftSecondaryToolbarHolder);
  left_sec_toolbar_layout->setContentsMargins(5, 0, 5, 0);

  _left_sec_toolbar = new Noggit::Ui::Tools::ViewToolbar::Ui::ViewToolbar(this, terrainMode);
  connect(this, &QObject::destroyed, _left_sec_toolbar, &QObject::deleteLater);
  left_sec_toolbar_layout->addWidget( _left_sec_toolbar);

  auto top_toolbar_layout = new QVBoxLayout(_viewport_overlay_ui->upperToolbarHolder);
  top_toolbar_layout->setContentsMargins(5, 0, 5, 0);
  auto sec_toolbar_layout = new QVBoxLayout(_viewport_overlay_ui->secondaryToolbarHolder);
  sec_toolbar_layout->setContentsMargins(5, 0, 5, 0);

  _viewport_overlay_ui->secondaryToolbarHolder->hide();
  _secondary_toolbar = new Noggit::Ui::Tools::ViewToolbar::Ui::ViewToolbar(this);
  connect (this, &QObject::destroyed, _secondary_toolbar, &QObject::deleteLater);

  _view_toolbar = new Noggit::Ui::Tools::ViewToolbar::Ui::ViewToolbar(this, _secondary_toolbar);
  connect (this, &QObject::destroyed, _view_toolbar, &QObject::deleteLater);

  top_toolbar_layout->addWidget( _view_toolbar);
  sec_toolbar_layout->addWidget( _secondary_toolbar);
}

void MapView::setupMainToolbar()
{
    _main_window->_app_toolbar = new QToolBar("Menu Toolbar", this); // this or mainwindow as parent?
    connect(this, &QObject::destroyed, _main_window->_app_toolbar, &QObject::deleteLater);

    _main_window->_app_toolbar->setOrientation(Qt::Horizontal);
    _main_window->addToolBar(_main_window->_app_toolbar);
    _main_window->_app_toolbar->setVisible(_settings->value("map_view/app_toolbar", false).toBool()); // hide by default.

    connect(_main_window->_app_toolbar, &QToolBar::visibilityChanged,
        [=](bool visible)
        {
            if (ui_hidden)
                return;

            _settings->setValue("map_view/app_toolbar", visible);
            _settings->sync();
        });

    // TODO
    /*
    auto save_changed_btn = new QPushButton(this);
    save_changed_btn->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::save));
    save_changed_btn->setToolTip("Save Changed");
    // save_changed_btn->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_S));
    _main_window->_app_toolbar->addWidget(save_changed_btn);

    auto undo_btn = new QPushButton(this);
    undo_btn->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::undo));
    undo_btn->setToolTip("Undo");
    // undo_btn->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Z));
    _main_window->_app_toolbar->addWidget(undo_btn);

    auto redo_btn = new QPushButton(this);
    redo_btn->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::redo));
    redo_btn->setToolTip("Undo");
    // redo_btn->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z));
    _main_window->_app_toolbar->addWidget(redo_btn);

    _main_window->_app_toolbar->addSeparator();

    QAction* start_server_action = _main_window->_app_toolbar->addAction("Start Server");
    start_server_action->setToolTip("Start World and Auth servers.");
    start_server_action->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::server));
    
    QAction* extract_server_map_action = _main_window->_app_toolbar->addAction("Extract Server Map");
    extract_server_map_action->setToolTip("Start server extractors for this map.");
    // TODO idea : detect modified tiles and only extract those.
    extract_server_map_action->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::map));
*/

    auto build_data_btn = new QPushButton(this); 
    _main_window->_app_toolbar->addWidget(build_data_btn);
    build_data_btn->setToolTip("Save content of project folder as MPQ patch in the client.");
    build_data_btn->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::filearchive));
    connect(build_data_btn, &QPushButton::clicked
        , [=]()
        {
            _main_window->patchWowClient(); // code to open dialog

        });

    auto start_wow_btn = new QPushButton(this);
    start_wow_btn->setIcon(Noggit::Ui::FontAwesomeIcon(Noggit::Ui::FontAwesome::play));
    start_wow_btn->setToolTip("Launch the client");
    _main_window->_app_toolbar->addWidget(start_wow_btn);

    connect(start_wow_btn, &QPushButton::clicked
        , [=]()
        {
            _main_window->startWowClient();
        });


    // TODO : restart button while WoW is running?

  // IDEAs : various client utils like synchronize client view with noggit, reload, patch WoW.exe with community patches like unlock md5 check, set WoW client version
}

std::unique_ptr<Noggit::Tool>& MapView::activeTool()
{
    return _tools[_activeToolIndex];
}

void MapView::activeTool(editing_mode newTool)
{
    for (size_t i = 0; i < _tools.size(); ++i)
    {
        if (_tools[i]->editingMode() == newTool)
        {
            _activeToolIndex = i;
            return;
        }
    }

    throw std::exception{ std::format("Tried to call MapView::activeTool with invalid editing_mode `{}`!", static_cast<int>(newTool)).c_str() };
}

Noggit::Ui::Tools::ViewToolbar::Ui::ViewToolbar* MapView::getLeftSecondaryViewToolbar()
{
    return _left_sec_toolbar;
}

QSettings* MapView::settings()
{
    return _settings;
}

Noggit::Ui::Windows::NoggitWindow* MapView::mainWindow()
{
    return _main_window;
}

bool MapView::isUiHidden() const
{
    return ui_hidden;
}

bool MapView::drawAdtGrid() const
{
    return _draw_lines.get();
}

bool MapView::drawHoleGrid() const
{
    return _draw_hole_lines.get();
}

void MapView::invalidate()
{
    _needs_redraw = true;
}

void MapView::selectObjects(std::array<glm::vec2, 2> selection_box, float depth)
{
    _world->select_objects_in_area(selection_box, !_mod_shift_down, _model_view, _projection, width(), height(), depth, _camera.position);
}

std::shared_ptr<Noggit::Project::NoggitProject>& MapView::project()
{
    return _project;
}

float MapView::timeSpeed() const
{
    return mTimespeed;
}

void MapView::setupKeybindingsGui()
{
  _keybindings = new Noggit::Ui::help(this);
  _keybindings->hide();
  connect(this, &QObject::destroyed, _keybindings, &QObject::deleteLater);

  connect ( &_show_keybindings_window, &Noggit::BoolToggleProperty::changed
    , _keybindings, &QWidget::setVisible
  );

  connect ( _keybindings, &Noggit::Ui::widget::visibilityChanged
    , &_show_keybindings_window, &Noggit::BoolToggleProperty::set
  );
}

void MapView::setupFileMenu()
{
  auto file_menu (_main_window->_menuBar->addMenu ("Editor"));
  connect (this, &QObject::destroyed, file_menu, &QObject::deleteLater);

  ADD_ACTION (file_menu, "Save current tile", "Ctrl+Shift+S", [this] { save(save_mode::current); emit saved();});
  ADD_ACTION (file_menu, "Save changed tiles", QKeySequence::Save, [this] { save(save_mode::changed); emit saved(); });
  ADD_ACTION (file_menu, "Save all tiles", "Ctrl+Shift+A", [this] { save(save_mode::all); emit saved(); });
  ADD_ACTION(file_menu, "Generate new WDL", "", [this] 
      { 
     QMessageBox prompt;
    prompt.setIcon(QMessageBox::Warning);
    prompt.setWindowFlags(Qt::WindowStaysOnTopHint);
     prompt.setText(std::string("Warning!\nThis will attempt to load all tiles in the map to generate a new WDL."
         "\nThis is likely to crash if there is any issue with any tile, it is recommended that you save your work first. Only use this if you really need a fresh WDL.").c_str());
     prompt.setInformativeText(std::string("Are you sure ?").c_str());
     prompt.setStandardButtons(QMessageBox::StandardButton::Yes | QMessageBox::StandardButton::No);
     prompt.setDefaultButton(QMessageBox::No);
     bool answer = prompt.exec() == QMessageBox::StandardButton::Yes;
     if (answer)
         _world->horizon.save_wdl(_world.get(), true);
      }
  );

  ADD_ACTION ( file_menu
  , "Reload tile"
  , "Shift+J"
  , [this]
               {
                 makeCurrent();
                 OpenGL::context::scoped_setter const _ (::gl, context());
                 _world->reload_tile (_camera.position);
                 emit rotationChanged();
                 emit saved();
               }
  );

  file_menu->addSeparator();
  ADD_ACTION_NS (file_menu, "Force uid check on next opening", [this] { _force_uid_check = true; });
  file_menu->addSeparator();

  ADD_ACTION ( file_menu
  , "Add bookmark"
  , Qt::CTRL | Qt::Key_F5
      , [this]
      {

          auto bookmark = Noggit::Project::NoggitProjectBookmarkMap();
          bookmark.position = _camera.position;
          bookmark.camera_pitch = _camera.pitch()._;
          bookmark.camera_yaw = _camera.yaw()._;
          bookmark.map_id = _world->getMapID();
          bookmark.name = gAreaDB.getAreaFullName(_world->getAreaID(_camera.position));

        _project->createBookmark(bookmark);

      }
  );

  ADD_ACTION(file_menu
      , "Write coordinates to port.txt and copy to clipboard"
      , Qt::Key_G
      , [this]
      {
                 std::stringstream port_command;
                 port_command << ".go XYZ " << (ZEROPOINT - _camera.position.z) << " " << (ZEROPOINT - _camera.position.x) << " " << _camera.position.y << " " << _world->getMapID();
                 std::ofstream f("ports.txt", std::ios_base::app);
                 f << "Map: " << gAreaDB.getAreaFullName(_world->getAreaID (_camera.position)) << " on ADT " << std::floor(_camera.position.x / TILESIZE) << " " << std::floor(_camera.position.z / TILESIZE) << std::endl;
                 f << "Trinity/AC:" << std::endl << port_command.str() << std::endl;
                 // f << "ArcEmu:" << std::endl << ".worldport " << _world->getMapID() << " " << (ZEROPOINT - _camera.position.z) << " " << (ZEROPOINT - _camera.position.x) << " " << _camera.position.y << " " << std::endl << std::endl;
                 f.close();
                 QClipboard* clipboard = QGuiApplication::clipboard();
                 clipboard->setText(port_command.str().c_str(), QClipboard::Clipboard);
               }
  );

}

void MapView::setupEditMenu()
{
  auto edit_menu (_main_window->_menuBar->addMenu ("Edit"));
  connect (this, &QObject::destroyed, edit_menu, &QObject::deleteLater);

  edit_menu->addSeparator();
  edit_menu->addAction(createTextSeparator("Selected object"));
  edit_menu->addSeparator();
  ADD_ACTION(edit_menu, "Delete", Qt::Key_Delete, [this]
    {
      if (get_editing_mode() == editing_mode::object)
      {
        NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_REMOVED);
        DeleteSelectedObjects();
        NOGGIT_ACTION_MGR->endAction();
      }
      else
      {
        for (auto&& hotkey : hotkeys)
        {
          if (Qt::Key_Delete == hotkey.key && hotkey.condition())
          {
            makeCurrent();
            OpenGL::context::scoped_setter const _(::gl, context());

            hotkey.onPress();
            return;
          }
        }
      }
    }
  );

  ADD_ACTION (edit_menu, "Reset rotation", "Ctrl+R",
              [this]
              {
                NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED);
                ResetSelectedObjectRotation();
                NOGGIT_ACTION_MGR->endAction();
              });
  ADD_ACTION (edit_menu, "Set to ground", Qt::Key_PageDown,
              [this] {
                NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED);
                snap_selected_models_to_the_ground();
                NOGGIT_ACTION_MGR->endAction();

              });

  edit_menu->addSeparator();
  edit_menu->addAction(createTextSeparator("Options"));
  edit_menu->addSeparator();
  ADD_TOGGLE_NS (edit_menu, "Locked cursor mode", _locked_cursor_mode);

  edit_menu->addSeparator();
  edit_menu->addAction(createTextSeparator("State"));
  edit_menu->addSeparator();
  ADD_ACTION (edit_menu, "Undo", "Ctrl+Z", [this] { NOGGIT_ACTION_MGR->undo(); });
  ADD_ACTION (edit_menu, "Redo", "Ctrl+Shift+Z", [this] { NOGGIT_ACTION_MGR->redo(); });
}

void MapView::setupAssistMenu()
{
  auto assist_menu (_main_window->_menuBar->addMenu ("Assist"));
  connect (this, &QObject::destroyed, assist_menu, &QObject::deleteLater);

  assist_menu->addSeparator();
  assist_menu->addAction(createTextSeparator("Current ADT"));
  assist_menu->addSeparator();

  ADD_ACTION_NS ( assist_menu
  , "Ensure 4 texture layers"
  , [=]
    {
      makeCurrent();
      OpenGL::context::scoped_setter const _(::gl, context());

      NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TEXTURE);
      _world->ensureAllTilesetsADT(_camera.position);
      NOGGIT_ACTION_MGR->endAction();

    }
  );

  auto cleanup_menu (assist_menu->addMenu ("Clean up"));

  ADD_ACTION_NS ( cleanup_menu
  , "Clear height map"
  , [this]
                  {
                    makeCurrent();
                    OpenGL::context::scoped_setter const _ (::gl, context());
                    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN);
                    _world->clearHeight(_camera.position);
                    NOGGIT_ACTION_MGR->endAction();
                  }
  );
  ADD_ACTION_NS ( cleanup_menu
  , "Remove texture duplicates"
  , [this]
                  {
                    makeCurrent();
                    OpenGL::context::scoped_setter const _ (::gl, context());
                    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TEXTURE);
                    _world->removeTexDuplicateOnADT(_camera.position);
                    NOGGIT_ACTION_MGR->endAction();
                  }
  );
  ADD_ACTION_NS ( cleanup_menu
  , "Clear textures"
  , [this]
                  {
                    makeCurrent();
                    OpenGL::context::scoped_setter const _ (::gl, context());
                    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TEXTURE);
                    _world->clearTextures(_camera.position);
                    NOGGIT_ACTION_MGR->endAction();
                  }
  );
  ADD_ACTION_NS ( cleanup_menu
  , "Clear textures + set base"
  , [this]
                  {
                    makeCurrent();
                    OpenGL::context::scoped_setter const _ (::gl, context());
                    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TEXTURE);
                    _world->setBaseTexture(_camera.position);
                    NOGGIT_ACTION_MGR->endAction();
                  }
  );
  ADD_ACTION_NS ( cleanup_menu
  , "Clear shadows"
  , [this]
                  {
                    makeCurrent();
                    OpenGL::context::scoped_setter const _ (::gl, context());
                    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNK_SHADOWS);
                    _world->clear_shadows(_camera.position);
                    NOGGIT_ACTION_MGR->endAction();
                  }
  );
  ADD_ACTION_NS ( cleanup_menu
  , "Clear models"
  , [this]
                  {
                    makeCurrent();
                    OpenGL::context::scoped_setter const _ (::gl, context());
                    NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_REMOVED);
                    _world->clearAllModelsOnADT(_camera.position, true);
                    NOGGIT_ACTION_MGR->endAction();
                    emit rotationChanged();
                  }
  );
  ADD_ACTION_NS ( cleanup_menu
  , "Clear duplicate models"
  , [this]
                  {
                    DESTRUCTIVE_ACTION
                      (
                        makeCurrent();
                    OpenGL::context::scoped_setter const _(::gl, context());
                    _world->delete_duplicate_model_and_wmo_instances();
                    )
                  }
  );

  auto cur_adt_export_menu(assist_menu->addMenu("Export"));
  ADD_ACTION_NS ( cur_adt_export_menu
  , "Export alphamaps"
  , [this]
  {
    makeCurrent();
    OpenGL::context::scoped_setter const _(::gl, context());
    _world->exportADTAlphamap(_camera.position);
  }
  );

  ADD_ACTION_NS ( cur_adt_export_menu
  , "Export alphamaps (current texture)"
  , [this]
  {
    makeCurrent();
    OpenGL::context::scoped_setter const _(::gl, context());

    if (!!Noggit::Ui::selected_texture::get())
    {
      _world->exportADTAlphamap(_camera.position, Noggit::Ui::selected_texture::get()->get()->file_key().filepath());
    }

  }
  );

  ADD_ACTION_NS ( cur_adt_export_menu
  , "Export vertex color map"
  , [this]
                  {
                    makeCurrent();
                    OpenGL::context::scoped_setter const _(::gl, context());

                    _world->exportADTVertexColorMap(_camera.position);
                  }
  );

  // vertices can support up to 32bit but other things break at 16bit like WDL and MFBO
  //  DB/ZoneLight appears to be using -64000 and 64000
  //  DB/DungeonMapChunk seems to use -10000 for lower default.
  int constexpr MIN_HEIGHT = std::numeric_limits<short>::min(); // -32768
  int constexpr MAX_HEIGHT = std::numeric_limits<short>::max(); // 32768

  int constexpr DEFAULT_MIN_HEIGHT = -2000; // outland goes to -1200
  int constexpr DEFAULT_MAX_HEIGHT = 3000; // hyjal goes to 2000

  QDialog* heightmap_export_params = new QDialog(this);
  heightmap_export_params->setWindowFlags(Qt::Popup);
  heightmap_export_params->setWindowTitle("Heightmap Exporter");
  QVBoxLayout* heightmap_export_params_layout = new QVBoxLayout(heightmap_export_params);

  heightmap_export_params_layout->addWidget(new QLabel("Import with the same values \nto keep the same coordinates.",
      heightmap_export_params));

  heightmap_export_params_layout->addWidget(new QLabel("Min Height:", heightmap_export_params));
  QDoubleSpinBox* heightmap_export_min = new QDoubleSpinBox(heightmap_export_params);
  heightmap_export_min->setRange(MIN_HEIGHT, MAX_HEIGHT);
  heightmap_export_min->setValue(DEFAULT_MIN_HEIGHT);
  heightmap_export_params_layout->addWidget(heightmap_export_min);

  heightmap_export_params_layout->addWidget(new QLabel("Max Height:", heightmap_export_params));
  QDoubleSpinBox* heightmap_export_max = new QDoubleSpinBox(heightmap_export_params);
  heightmap_export_max->setRange(MIN_HEIGHT, MAX_HEIGHT);
  heightmap_export_max->setValue(DEFAULT_MAX_HEIGHT);
  heightmap_export_params_layout->addWidget(heightmap_export_max);

  std::string const autoheights_tooltip_str = "Sets fields to this tile's min and max heights\nDefaults : Min: "
      + std::to_string(DEFAULT_MIN_HEIGHT) + ", Max: " + std::to_string(DEFAULT_MAX_HEIGHT);
  QPushButton* heightmap_export_params_auto_height = new QPushButton("Auto Heights", heightmap_export_params);
  heightmap_export_params_auto_height->setToolTip(autoheights_tooltip_str.c_str());
  heightmap_export_params_layout->addWidget(heightmap_export_params_auto_height);

  QPushButton* heightmap_export_okay = new QPushButton("Okay", heightmap_export_params);
  heightmap_export_params_layout->addWidget(heightmap_export_okay);

  connect(heightmap_export_min, qOverload<double>(&QDoubleSpinBox::valueChanged),
          [=](double value)
          {
            if (!(heightmap_export_max->value() > value))
              heightmap_export_max->setValue(value + 1.0);

          });

  connect(heightmap_export_max, qOverload<double>(&QDoubleSpinBox::valueChanged),
          [=](double value)
          {
            if (!(heightmap_export_min->value() < value))
              heightmap_export_min->setValue(value - 1.0);

          });

  connect(heightmap_export_params_auto_height, &QPushButton::clicked
      , [=]()
      {
          MapTile* tile = _world->mapIndex.getTile(_camera.position);
          if (tile)
          {
              QSignalBlocker const blocker_min(heightmap_export_min);
              QSignalBlocker const blocker_max(heightmap_export_max);

              heightmap_export_min->setValue(tile->getMinHeight());
              heightmap_export_max->setValue(tile->getMaxHeight());
          }
      });

  connect(heightmap_export_okay, &QPushButton::clicked
    ,[=]()
    {
      heightmap_export_params->accept();

    });



  ADD_ACTION_NS ( cur_adt_export_menu
  , "Export heightmap"
  , [=]
              {
                QPoint new_pos = QCursor::pos();

                heightmap_export_params->setGeometry(new_pos.x(),
                new_pos.y(),
                heightmap_export_params->width(),
                heightmap_export_params->height());

                if (heightmap_export_params->exec() == QDialog::Accepted)
                {
                  makeCurrent();
                  OpenGL::context::scoped_setter const _(::gl, context());

                  _world->exportADTHeightmap(_camera.position, heightmap_export_min->value(), heightmap_export_max->value());
                }

              }
  );

  ADD_ACTION_NS ( cur_adt_export_menu
  , "Export normalmap"
  , [this]
      {
        makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, context());
        _world->exportADTNormalmap(_camera.position);
      }
  );

  auto cur_adt_import_menu(assist_menu->addMenu("Import"));

  // alphamaps import
  auto const alphamap_image_format = "Required Image format :\n1024x1024 and 8bit color channel.";

  QDialog* adt_import_params = new QDialog(this);
  adt_import_params->setWindowFlags(Qt::Popup);
  adt_import_params->setWindowTitle("Alphamap Importer");
  QVBoxLayout* adt_import_params_layout = new QVBoxLayout(adt_import_params);

  adt_import_params_layout->addWidget(new QLabel("Layer:", adt_import_params));
  QSpinBox* adt_import_params_layer = new QSpinBox(adt_import_params);
  adt_import_params_layer->setRange(1, 3);
  adt_import_params_layout->addWidget(adt_import_params_layer);

  QCheckBox* adt_import_params_cleanup_layers = new QCheckBox("Cleanup unused chunk layers", adt_import_params);
  adt_import_params_cleanup_layers->setToolTip("Remove textures that have empty layers from chunks.");
  adt_import_params_cleanup_layers->setChecked(false);
  adt_import_params_layout->addWidget(adt_import_params_cleanup_layers);

  QPushButton* adt_import_params_okay = new QPushButton("Okay", adt_import_params);
  adt_import_params_layout->addWidget(adt_import_params_okay);

  auto const alphamap_file_info_tooltip = "\nThe image file must be placed in the map's directory in the project"
      " folder with the following naming : MAPNAME_XX_YY_layer1.png (or layer2...)."
      "\nFor example \"C:/noggitproject/world/maps/MAPNAME/MAPNAME_29_53_layer2.png\"";
  adt_import_params_okay->setToolTip(alphamap_file_info_tooltip);

  connect(adt_import_params_okay, &QPushButton::clicked
    ,[=]()
    {
      adt_import_params->accept();

    });

  ADD_ACTION_NS ( cur_adt_import_menu
  , "Import alphamap (file)"
  , [=]
                  {
                    QPoint new_pos = QCursor::pos();

                    adt_import_params->setGeometry(new_pos.x(),
                                                   new_pos.y(),
                                                   heightmap_export_params->width(),
                                                   heightmap_export_params->height());

                    if (adt_import_params->exec() == QDialog::Accepted)
                    {
                      makeCurrent();
                      OpenGL::context::scoped_setter const _(::gl, context());

                      QString filepath = QFileDialog::getOpenFileName(
                        this,
                        tr("Open alphamap"),
                        "",
                        "PNG file (*.png);;"
                      );

                      if(!QFileInfo::exists(filepath))
                        return;

                      QImage img;
                      img.load(filepath, "PNG");

                      NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TEXTURE);
                      _world->importADTAlphamap(_camera.position, img, adt_import_params_layer->value(), adt_import_params_cleanup_layers->isChecked());
                      NOGGIT_ACTION_MGR->endAction();
                    }

                  }
  );

  ADD_ACTION_NS ( cur_adt_import_menu
  , "Import alphamaps"
  , [=]
    {

        makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, context());

        NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TEXTURE);
        _world->importADTAlphamap(_camera.position, adt_import_params_cleanup_layers->isChecked());
        NOGGIT_ACTION_MGR->endAction();
    }
  );

  auto const heightmap_image_format = "Required Image format :\n257x257 or 256x256(tiled edges)\nand 16bit per color channel.";

  auto const heightmap_file_info_tooltip = "Requires a .png image of 257x257, or 256x256 in Tiled Edges mode.(Otherwise it will be stretched)"
      "\nThe image file must be placed in the map's directory in the project folder with the following naming : MAPNAME_XX_YY_height.png."
      "\nFor example \"C:/noggitproject/world/maps/MAPNAME/MAPNAME_29_53_height.png\"";

  auto const tiled_edges_tooltip_str = "Tiled edge uses a 256x256 image instead 257."
      "\nTiled image imports encroach on edge vertices on neighboring tiles to avoid duplicate edges. ";

  /*auto const multiplier_tooltip_str = "Multiplies pixel values by this to obtain the final position."
      "\n For example a pixel grayscale of 40%(0.4%) with a multiplier of 100 means this vertex's height will be 0.4*100 = 40.";
*/

  // heightmaps
  QDialog* adt_import_height_params = new QDialog(this);
  adt_import_height_params->setWindowFlags(Qt::Popup);
  adt_import_height_params->setWindowTitle("Heightmap Importer");
  QVBoxLayout* adt_import_height_params_layout = new QVBoxLayout(adt_import_height_params);

  adt_import_height_params_layout->addWidget(new QLabel(heightmap_image_format, adt_import_height_params));

  adt_import_height_params_layout->addWidget(new QLabel("Min Height:", adt_import_height_params));
  QDoubleSpinBox* heightmap_import_min = new QDoubleSpinBox(adt_import_height_params);
  heightmap_import_min->setRange(MIN_HEIGHT, MAX_HEIGHT);
  heightmap_import_min->setValue(DEFAULT_MIN_HEIGHT);
  adt_import_height_params_layout->addWidget(heightmap_import_min);

  adt_import_height_params_layout->addWidget(new QLabel("Max Height:", adt_import_height_params));
  QDoubleSpinBox* heightmap_import_max = new QDoubleSpinBox(adt_import_height_params);
  heightmap_import_max->setRange(MIN_HEIGHT, MAX_HEIGHT);
  heightmap_import_max->setValue(DEFAULT_MAX_HEIGHT);
  adt_import_height_params_layout->addWidget(heightmap_import_max);

  QPushButton* adt_import_height_params_auto_height = new QPushButton("Auto Heights", adt_import_height_params);
  adt_import_height_params_auto_height->setToolTip(autoheights_tooltip_str.c_str());
  adt_import_height_params_layout->addWidget(adt_import_height_params_auto_height);

  adt_import_height_params_layout->addWidget(new QLabel("Mode:", adt_import_height_params));
  QComboBox* adt_import_height_params_mode = new QComboBox(adt_import_height_params);
  adt_import_height_params_layout->addWidget(adt_import_height_params_mode);
  adt_import_height_params_mode->addItems({"Set", "Add", "Subtract", "Multiply" });

  QCheckBox* adt_import_height_tiled_edges = new QCheckBox("Tiled Edges", adt_import_height_params);
  adt_import_height_tiled_edges->setToolTip(tiled_edges_tooltip_str);
  adt_import_height_params_layout->addWidget(adt_import_height_tiled_edges);

  QPushButton* adt_import_height_params_okay = new QPushButton("Okay", adt_import_height_params);
  adt_import_height_params_layout->addWidget(adt_import_height_params_okay);
  adt_import_height_params_okay->setToolTip(heightmap_file_info_tooltip);

  connect(adt_import_height_params_auto_height, &QPushButton::clicked
    , [=]()
    {
      MapTile* tile = _world->mapIndex.getTile(_camera.position);
      if (tile)
      {
        heightmap_import_min->setValue(tile->getMinHeight());
        heightmap_import_max->setValue(tile->getMaxHeight());
      }
    });

  connect(adt_import_height_params_okay, &QPushButton::clicked
    ,[=]()
          {
            adt_import_height_params->accept();

          });

  ADD_ACTION_NS ( cur_adt_import_menu
  , "Import heightmap (file)"
  , [=]
      {
        if (adt_import_height_params->exec() == QDialog::Accepted)
        {
          makeCurrent();
          OpenGL::context::scoped_setter const _(::gl, context());

          QString filepath = QFileDialog::getOpenFileName(
            this,
            tr("Open heightmap (257x257)"),
            "",
            "PNG file (*.png);;"
          );

          if(!QFileInfo::exists(filepath))
            return;

          QImage img;
          img.load(filepath, "PNG");

          NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN);
          _world->importADTHeightmap(_camera.position, img, heightmap_import_min->value(), heightmap_import_max->value(),
                                     adt_import_height_params_mode->currentIndex(), adt_import_height_tiled_edges->isChecked());
          NOGGIT_ACTION_MGR->endAction();
        }
      }
  );

  ADD_ACTION_NS ( cur_adt_import_menu
  , "Import heightmap"
  , [=]
      {
        if (adt_import_height_params->exec() == QDialog::Accepted)
        {
          makeCurrent();
          OpenGL::context::scoped_setter const _(::gl, context());

          NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN);
          _world->importADTHeightmap(_camera.position, heightmap_import_min->value(), heightmap_import_max->value(),
                                     adt_import_height_params_mode->currentIndex(), adt_import_height_tiled_edges->isChecked());
          NOGGIT_ACTION_MGR->endAction();
        }
      }
  );

  // Watermap
  QDialog* adt_import_water_params = new QDialog(this);
  adt_import_water_params->setWindowFlags(Qt::Popup);
  adt_import_water_params->setWindowTitle("Watermap Importer");
  QVBoxLayout* adt_import_water_params_layout = new QVBoxLayout(adt_import_water_params);

  // MIN MAX
  adt_import_water_params_layout->addWidget(new QLabel("Min Height:", adt_import_water_params));
  QDoubleSpinBox* watermap_import_min = new QDoubleSpinBox(adt_import_water_params);
  watermap_import_min->setRange(MIN_HEIGHT, MAX_HEIGHT);
  watermap_import_min->setValue(MIN_HEIGHT);
  adt_import_water_params_layout->addWidget(watermap_import_min);

  adt_import_water_params_layout->addWidget(new QLabel("Max Height:", adt_import_water_params));
  QDoubleSpinBox* watermap_import_max = new QDoubleSpinBox(adt_import_water_params);
  watermap_import_max->setRange(MIN_HEIGHT, MAX_HEIGHT);
  watermap_import_max->setValue(MAX_HEIGHT);
  adt_import_water_params_layout->addWidget(watermap_import_max);

  adt_import_water_params_layout->addWidget(new QLabel("Mode:", adt_import_water_params));
  QComboBox* adt_import_water_params_mode = new QComboBox(adt_import_water_params);
  adt_import_water_params_layout->addWidget(adt_import_water_params_mode);
  adt_import_water_params_mode->addItems({ "Set", "Add", "Subtract", "Multiply" });

  QCheckBox* adt_import_water_tiled_edges = new QCheckBox("Tiled Edges", adt_import_water_params);
  adt_import_water_params_layout->addWidget(adt_import_water_tiled_edges);

  QPushButton* adt_import_water_params_okay = new QPushButton("Okay", adt_import_water_params);
  adt_import_water_params_layout->addWidget(adt_import_water_params_okay);

  connect(adt_import_water_params_okay, &QPushButton::clicked
      , [=]()
      {
          adt_import_water_params->accept();

      });

  ADD_ACTION_NS(cur_adt_import_menu
      , "Import watermap (file)"
      , [=]
      {
          if (adt_import_water_params->exec() == QDialog::Accepted)
          {
              makeCurrent();
              OpenGL::context::scoped_setter const _(::gl, context());

              QString filepath = QFileDialog::getOpenFileName(
                  this,
                  tr("Open watermap (257x257)"),
                  "",
                  "PNG file (*.png);;"
              );

              if (!QFileInfo::exists(filepath))
                  return;

              QImage img;
              img.load(filepath, "PNG");

              NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_WATER);
              _world->importADTWatermap(_camera.position, img, watermap_import_min->value(), watermap_import_max->value(),
                  adt_import_water_params_mode->currentIndex(), adt_import_water_tiled_edges->isChecked());
              NOGGIT_ACTION_MGR->endAction();
          }
      }
  );

  // Vertex Colors
  QDialog* adt_import_vcol_params = new QDialog(this);
  adt_import_vcol_params->setWindowFlags(Qt::Popup);
  adt_import_vcol_params->setWindowTitle("Vertex Color Map Importer");
  QVBoxLayout* adt_import_vcol_params_layout = new QVBoxLayout(adt_import_vcol_params);

  adt_import_vcol_params_layout->addWidget(new QLabel("Mode:", adt_import_vcol_params));
  QComboBox* adt_import_vcol_params_mode = new QComboBox(adt_import_vcol_params);
  adt_import_vcol_params_layout->addWidget(adt_import_vcol_params_mode);
  adt_import_vcol_params_mode->addItems({"Set", "Add", "Subtract", "Multiply"});

  QCheckBox* adt_import_vcol_params_mode_tiled_edges = new QCheckBox("Tiled Edges", adt_import_vcol_params);
  adt_import_vcol_params_layout->addWidget(adt_import_vcol_params_mode_tiled_edges);

  QPushButton* adt_import_vcol_params_okay = new QPushButton("Okay", adt_import_vcol_params);
  adt_import_vcol_params_layout->addWidget(adt_import_vcol_params_okay);

  connect(adt_import_vcol_params_okay, &QPushButton::clicked
    ,[=]()
          {
            adt_import_vcol_params->accept();

          });


  ADD_ACTION_NS ( cur_adt_import_menu
  , "Import vertex color map (file)"
  , [=]
    {
      if (adt_import_vcol_params->exec() == QDialog::Accepted)
      {
        makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, context());

        QString filepath = QFileDialog::getOpenFileName(
          this,
          tr("Open vertex color map (257x257)"),
          "",
          "PNG file (*.png);;"
        );

        if(!QFileInfo::exists(filepath))
          return;

        QImage img;
        img.load(filepath, "PNG");

        NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_VERTEX_COLOR);
        _world->importADTVertexColorMap(_camera.position, img, adt_import_vcol_params_mode->currentIndex(), adt_import_vcol_params_mode_tiled_edges->isChecked());
        NOGGIT_ACTION_MGR->endAction();
      }
    }
  );

  ADD_ACTION_NS ( cur_adt_import_menu
  , "Import vertex color map"
  , [=]
      {
        if (adt_import_vcol_params->exec() == QDialog::Accepted)
        {
          makeCurrent();
          OpenGL::context::scoped_setter const _(::gl, context());

          NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_VERTEX_COLOR);
          _world->importADTVertexColorMap(_camera.position, adt_import_vcol_params_mode->currentIndex(), adt_import_vcol_params_mode_tiled_edges->isChecked());
          NOGGIT_ACTION_MGR->endAction();
        }
      }
  );


  assist_menu->addSeparator();
  assist_menu->addAction(createTextSeparator("Loaded ADTs"));
  assist_menu->addSeparator();
  ADD_ACTION_NS ( assist_menu
  , "Fix terrain gaps between chunks"
  , [this]
      {
        makeCurrent();
        OpenGL::context::scoped_setter const _ (::gl, context());
        NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN);
        _world->fixAllGaps();
        NOGGIT_ACTION_MGR->endAction();
      }
  );

  ADD_ACTION_NS(assist_menu
      , "Cleanup empty texture chunks"
      , [this]
      {
          makeCurrent();
          OpenGL::context::scoped_setter const _(::gl, context());
          NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TEXTURE);
          _world->CleanupEmptyTexturesChunks();
          NOGGIT_ACTION_MGR->endAction();
      }
  );

  assist_menu->addSeparator();
  assist_menu->addAction(createTextSeparator("Global"));
  assist_menu->addSeparator();
  ADD_ACTION_NS ( assist_menu
  , "Convert Map to 8bits alphamaps"
  , [this]
    {
      DESTRUCTIVE_ACTION
      (
        makeCurrent();
        OpenGL::context::scoped_setter const _ (::gl, context());
        if (_world->mapIndex.hasBigAlpha())
        {
            QMessageBox::information(this
                , "Noggit"
                , "Map is already Big Alpha."
                , QMessageBox::Ok
            );
        }
        else
        {
            QProgressDialog progress_dialog("Converting Alpha format...", "", 0, _world->mapIndex.getNumExistingTiles(), this);
            progress_dialog.setWindowModality(Qt::WindowModal);
            _world->convert_alphamap(&progress_dialog, true);
        }
      )
    }
  );

  ADD_ACTION_NS ( assist_menu
  , "Convert Map to 4bits alphamaps (old format)"
  , [this]
    {
      DESTRUCTIVE_ACTION
      (
        makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, context());
        if (!_world->mapIndex.hasBigAlpha())
        {
            QMessageBox::information(this
                , "Noggit"
                , "Map is already Old Alpha."
                , QMessageBox::Ok
            );
        }
        else
        {
            QProgressDialog progress_dialog("Converting Alpha format...", "", 0, _world->mapIndex.getNumExistingTiles(), this);
            // Modal like its three siblings (:3007 just above, :3131, :3149), and it has to be:
            // QProgressDialog::setValue only calls QCoreApplication::processEvents() when the
            // dialog isModal(). Without this line the dialog never processed an event for the
            // entire 4096-tile conversion -- it drew as a grey rectangle and Windows marked the
            // whole application "Not Responding" until the conversion finished.
            progress_dialog.setWindowModality(Qt::WindowModal);
            _world->convert_alphamap(&progress_dialog, false);
        }
      )
    }
  );


  ADD_ACTION_NS ( assist_menu
  , "Ensure 4 texture layers"
  , [=]
      {
        DESTRUCTIVE_ACTION
        (
          makeCurrent();
          OpenGL::context::scoped_setter const _(::gl, context());
          _world->ensureAllTilesetsAllADTs();
        )

      }
  );

  auto all_adts_export_menu(assist_menu->addMenu("Export"));

  ADD_ACTION_NS ( all_adts_export_menu
  , "Export alphamaps"
  , [this]
    {
      DESTRUCTIVE_ACTION
      (
        makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, context());
        _world->exportAllADTsAlphamap();
      )
    }
  );

  ADD_ACTION_NS ( all_adts_export_menu
  , "Export alphamaps (current texture)"
  , [this]
  {
    DESTRUCTIVE_ACTION
    (
      makeCurrent();
      OpenGL::context::scoped_setter const _(::gl, context());

      if (!!Noggit::Ui::selected_texture::get())
      {
        _world->exportAllADTsAlphamap(Noggit::Ui::selected_texture::get()->get()->file_key().filepath());
      }
    )
  }
  );

  ADD_ACTION_NS ( all_adts_export_menu
  , "Export heightmap"
  , [this]
    {
      DESTRUCTIVE_ACTION
      (
        makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, context());

        _world->exportAllADTsHeightmap();
      )
    }
  );

  ADD_ACTION_NS ( all_adts_export_menu
  , "Export vertex color map"
  , [this]
    {
      DESTRUCTIVE_ACTION
      (
        makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, context());

        _world->exportAllADTsVertexColorMap();
      )
    }
  );

  auto all_adts_import_menu(assist_menu->addMenu("Import"));

  ADD_ACTION_NS ( all_adts_import_menu
  , "Import alphamaps"
  , [this]
  {
    DESTRUCTIVE_ACTION
    (
        makeCurrent();
        OpenGL::context::scoped_setter const _(::gl, context());
        QProgressDialog progress_dialog("Importing Alphamaps...", "Cancel", 0, _world->mapIndex.getNumExistingTiles(), this);
        progress_dialog.setWindowModality(Qt::WindowModal);
        NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TEXTURE);
        _world->importAllADTsAlphamaps(&progress_dialog);
        NOGGIT_ACTION_MGR->endAction();
    )
  }
  );
  ADD_ACTION_NS ( all_adts_import_menu
  , "Import heightmaps"
  , [=]
    {
      if (adt_import_height_params->exec() == QDialog::Accepted)
      {
        DESTRUCTIVE_ACTION
        (
            makeCurrent();
            OpenGL::context::scoped_setter const _(::gl, context());
            QProgressDialog progress_dialog("Importing Heightmaps...", "Cancel", 0, _world->mapIndex.getNumExistingTiles(), this);
            progress_dialog.setWindowModality(Qt::WindowModal);
            NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_TERRAIN);
            _world->importAllADTsHeightmaps(&progress_dialog, heightmap_import_min->value(), heightmap_import_max->value(), 
                adt_import_height_params_mode->currentIndex(), adt_import_height_tiled_edges->isChecked());
            NOGGIT_ACTION_MGR->endAction();
        )

      }
    }
  );

  ADD_ACTION_NS ( all_adts_import_menu
  , "Import vertex color maps"
  , [=]
  {
    if (adt_import_vcol_params->exec() == QDialog::Accepted)
    {
      DESTRUCTIVE_ACTION
      (
          makeCurrent();
          OpenGL::context::scoped_setter const _(::gl, context());
          NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eCHUNKS_VERTEX_COLOR);
          _world->importAllADTVertexColorMaps(adt_import_vcol_params_mode->currentIndex(), adt_import_vcol_params_mode_tiled_edges->isChecked());
          NOGGIT_ACTION_MGR->endAction();
      )

    }
  }
  );

  // Bookmarks -> game_tele. The cheapest useful bridge between the editor and the server: a
  // place you marked while building becomes `.tele <name>` in game.
  {
    auto export_teles (new QAction("Export bookmarks as game_tele SQL...", this));
    export_teles->setStatusTip
      ("Writes a .sql file adding every bookmark on this map to game_tele.");
    assist_menu->addAction(export_teles);

    connect(export_teles, &QAction::triggered, this, [this]
      {
        std::vector<Noggit::Database::GameTele::Entry> entries;

        for (auto const& bookmark : _project->Bookmarks)
        {
          // Bookmarks span every map in the project; a tele row carries its own map id, but
          // exporting another map's bookmarks from here would be surprising.
          if (bookmark.map_id != static_cast<int>(_world->getMapID()))
          {
            continue;
          }

          Noggit::Database::NoggitPlacement placement;
          placement.x = bookmark.position.x;
          placement.y = bookmark.position.y;
          placement.z = bookmark.position.z;

          Noggit::Database::GameTele::Entry entry;
          entry.name = bookmark.name;
          entry.map = static_cast<std::uint16_t>(bookmark.map_id);
          entry.position = Noggit::Database::SpawnPlacement::serverPositionFor(placement);
          entry.orientation
            = Noggit::Database::GameTele::orientationFromCameraYaw(bookmark.camera_yaw);

          entries.push_back(std::move(entry));
        }

        if (entries.empty())
        {
          QMessageBox::information
            ( this
            , "Export game_tele"
            , "There are no bookmarks on this map. Add one with the bookmark action first."
            );
          return;
        }

        auto const result (Noggit::Database::GameTele::build(entries));

        QString const path
          ( QFileDialog::getSaveFileName
            ( this
            , "Export game_tele SQL"
            , QString::fromStdString(Noggit::Project::CurrentProject::get()->ProjectPath)
                + "/game_tele.sql"
            , "SQL (*.sql)"
            )
          );

        if (path.isEmpty())
        {
          return;
        }

        QFile file (path);

        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        {
          QMessageBox::critical(this, "Export game_tele", "Could not write " + path);
          return;
        }

        file.write(result.sql.data(), static_cast<qint64>(result.sql.size()));
        file.close();

        QString message
          (QString("%1 destination(s) written to %2.").arg(result.emitted).arg(path));

        // Skipped names are reported rather than dropped: a bookmark named "My Base" cannot
        // become a tele, and silently omitting it would look like the export lost it.
        if (!result.skipped.empty())
        {
          message += QString("\n\nSkipped %1:\n").arg(result.skipped.size());

          for (auto const& skipped : result.skipped)
          {
            message += "  " + QString::fromStdString(skipped) + "\n";
          }
        }

        QMessageBox::information(this, "Export game_tele", message);
        Log << message.toStdString() << std::endl;
      }
    );
  }

  // Missing asset report. Shipping without this means players get invisible objects and purple
  // textures, and the first you hear of it is a bug report.
  {
    auto scan_assets (new QAction("Report missing assets...", this));
    scan_assets->setStatusTip
      ("Every model and texture referenced by the loaded tiles but absent from the client data.");
    assist_menu->addAction(scan_assets);

    connect(scan_assets, &QAction::triggered, this, [this]
      {
        auto* client_data = Noggit::Application::NoggitApplication::instance()->clientData();

        if (!client_data)
        {
          QMessageBox::critical(this, "Missing assets", "No client data is loaded.");
          return;
        }

        Noggit::AssetScanner scanner
          (Noggit::AssetScanCollector::makeClientDataProbe(client_data));

        std::size_t tiles = 0;

        for (MapTile* tile : _world->mapIndex.loaded_tiles())
        {
          Noggit::AssetScanCollector::collectTile<MapTile, Model, WMO>(tile, scanner);
          ++tiles;
        }

        auto const& result = scanner.result();

        QString report
          ( QString("Scanned %1 loaded tile(s).\n\n%2\n")
              .arg(tiles).arg(QString::fromStdString(result.summary())));

        if (!result.hasFailures())
        {
          QMessageBox::information(this, "Missing assets", report + "\nNothing is missing.");
          return;
        }

        for (auto const& failure : result.failures())
        {
          // display_path, not key: the reporter has to go and find this string in the ADT or the
          // DBC, and the normalised key appears nowhere in the data.
          report += QString("\n%1  (%2 reference%3)")
                      .arg(QString::fromStdString(failure.display_path))
                      .arg(failure.reference_count)
                      .arg(failure.reference_count == 1 ? "" : "s");
        }

        QString const path
          ( QFileDialog::getSaveFileName
            ( this
            , "Save missing asset report"
            , QString::fromStdString(Noggit::Project::CurrentProject::get()->ProjectPath)
                + "/missing-assets.txt"
            , "Text (*.txt)"
            )
          );

        if (!path.isEmpty())
        {
          QFile file (path);

          if (file.open(QIODevice::WriteOnly | QIODevice::Text))
          {
            QByteArray const bytes (report.toUtf8());
            file.write(bytes);
            file.close();
          }
        }

        Log << "Missing asset scan: " << result.summary() << std::endl;
      }
    );
  }

  // UID collision report. The renumbering already happens silently on every load; this is the
  // only way to find out what it moved.
  {
    auto uid_report (new QAction("UID collision report...", this));
    uid_report->setStatusTip
      ("Duplicate unique IDs repaired while loading. Silent corruption if left unexamined.");
    assist_menu->addAction(uid_report);

    connect(uid_report, &QAction::triggered, this, [this]
      {
        auto const& log = _world->uidCollisionLog();
        auto const records = log.snapshot();

        if (records.empty())
        {
          QMessageBox::information
            ( this
            , "UID collisions"
            , "No duplicate unique IDs were found while loading this map.\n\n"
              "That is the healthy result: every object kept the id it was saved with."
            );
          return;
        }

        QString report
          ( QString("%1 duplicate unique ID(s) were repaired while loading.\n\n"
                    "Each object below was renumbered in memory so it would not overwrite\n"
                    "another. Save the map to make the repair permanent.\n")
              .arg(log.totalCount()));

        if (log.truncated())
        {
          report += QString("\nOnly the first %1 are listed; the rest were not recorded.\n")
                      .arg(log.recordedCount());
        }

        for (auto const& record : records)
        {
          report += "\n" + QString::fromStdString(Noggit::formatUidCollision(record));
        }

        QMessageBox box (this);
        box.setWindowTitle("UID collisions");
        box.setIcon(QMessageBox::Warning);
        box.setText(QString("%1 duplicate unique ID(s) repaired.").arg(log.totalCount()));
        box.setDetailedText(report);
        box.exec();

        Log << "UID collision report: " << log.totalCount() << " repaired" << std::endl;
      }
    );
  }

  auto debug_menu(assist_menu->addMenu("Debug"));

  ADD_ACTION_NS ( debug_menu
  , "Load all tiles"
  , [=]
  {
    makeCurrent();
    OpenGL::context::scoped_setter const _(::gl, context());
    _unload_tiles = false;
    _world->loadAllTiles(_camera.position);
  }
  );

}

void MapView::setupViewMenu()
{
  auto view_menu (_main_window->_menuBar->addMenu ("View"));
  connect (this, &QObject::destroyed, view_menu, &QObject::deleteLater);

  view_menu->addSeparator();
  view_menu->addAction(createTextSeparator("Drawing"));
  view_menu->addSeparator();
  ADD_TOGGLE (view_menu, "Doodads",     Qt::Key_F1, _draw_models);
  ADD_TOGGLE (view_menu, "WMO doodads", Qt::Key_F2, _draw_wmo_doodads);
  ADD_TOGGLE (view_menu, "Terrain",     Qt::Key_F3, _draw_terrain);
  ADD_TOGGLE (view_menu, "Water",       Qt::Key_F4, _draw_water);
  ADD_TOGGLE (view_menu, "WMOs",        Qt::Key_F6, _draw_wmo);

  ADD_TOGGLE_POST (view_menu, "Lines", Qt::Key_F7, _draw_lines,
                   [=]
                   {
                     _world->renderer()->getTerrainParamsUniformBlock()->draw_lines = _draw_lines.get();
                     _world->renderer()->markTerrainParamsUniformBlockDirty();
                   });

  ADD_TOGGLE_POST (view_menu, "Contours", Qt::Key_F9, _draw_contour,
                   [=]
                   {
                     _world->renderer()->getTerrainParamsUniformBlock()->draw_terrain_height_contour = _draw_contour.get();
                     _world->renderer()->markTerrainParamsUniformBlockDirty();
                   });

  ADD_TOGGLE_POST (view_menu, "Wireframe", Qt::Key_F10, _draw_wireframe,
                   [=]
                   {
                     _world->renderer()->getTerrainParamsUniformBlock()->draw_wireframe = _draw_wireframe.get();
                     _world->renderer()->markTerrainParamsUniformBlockDirty();
                   });

  ADD_TOGGLE (view_menu, "Toggle Animation", Qt::Key_F11, _draw_model_animations);
  ADD_TOGGLE (view_menu, "Draw fog", Qt::Key_F12, _draw_fog);

  ADD_TOGGLE_POST (view_menu, "Hole lines", Qt::SHIFT | Qt::Key_F1, _draw_hole_lines,
                   [=]
                   {
                     _world->renderer()->getTerrainParamsUniformBlock()->draw_hole_lines = _draw_hole_lines.get();
                     _world->renderer()->markTerrainParamsUniformBlockDirty();
                   });

  ADD_TOGGLE_POST(view_menu, "Climb", Qt::SHIFT | Qt::Key_F2, _draw_climb,
                  [=]
                  {
                      _world->renderer()->getTerrainParamsUniformBlock()->draw_impassible_climb = _draw_climb.get();
                      _world->renderer()->markTerrainParamsUniformBlockDirty();
                  });

  ADD_TOGGLE_POST(view_menu, "Vertex Color", Qt::SHIFT | Qt::Key_F3, _draw_vertex_color,
      [=]
      {
          _world->renderer()->getTerrainParamsUniformBlock()->draw_vertex_color = _draw_vertex_color.get();
          _world->renderer()->markTerrainParamsUniformBlockDirty();
      });

  ADD_TOGGLE_POST(view_menu, "Baked Shadows", Qt::SHIFT | Qt::Key_F4, _draw_baked_shadows,
      [=]
      {
          _world->renderer()->getTerrainParamsUniformBlock()->draw_shadows = _draw_baked_shadows.get();
          _world->renderer()->markTerrainParamsUniformBlockDirty();
      });

  ADD_TOGGLE_NS (view_menu, "Flight Bounds", _draw_mfbo);

  ADD_TOGGLE_NS (view_menu, "Models with box", _draw_models_with_box);
  //! \todo space+h in object mode
  ADD_TOGGLE_NS (view_menu, "Hidden models", _draw_hidden_models);

  // Placed next to "Models with box" because that is what it is: a box drawn instead of a model.
  //
  // Not an ADD_TOGGLE_NS, because that macro binds a Noggit::BoolToggleProperty member of
  // MapView and the state this one carries has to be readable from WorldRender::draw, which has
  // no MapView. It lives on MissingPlacementLog instead -- see the comment there, and the report
  // note about moving it into WorldRenderParams once that header is free.
  {
    auto* placeholders (new QAction("Missing model placeholders", this));
    placeholders->setCheckable(true);
    placeholders->setChecked(Noggit::MissingPlacementLog::instance().drawPlaceholders());
    placeholders->setStatusTip
      ("Draw a checkered cube where a model or WMO could not be loaded, so the placement can be "
       "seen and selected.");
    view_menu->addAction(placeholders);

    connect(placeholders, &QAction::toggled, this, [this] (bool on)
      {
        Noggit::MissingPlacementLog::instance().setDrawPlaceholders(on);
        _needs_redraw = true;
      });
  }

  ADD_TOGGLE_NS(view_menu, "Draw Sky", _draw_sky);
  ADD_TOGGLE_NS(view_menu, "Draw Skybox", _draw_skybox);

  // TrinityCore world-database spawn overlay.
  //
  // The toggle only controls visibility; the query is a separate explicit action, because a
  // per-tile database read must not be triggered by something as incidental as ticking a menu
  // item. Loading is also what tells the user whether the connection works at all, so it is
  // worth being a deliberate act with a reported result.
  view_menu->addSeparator();
  view_menu->addAction(createTextSeparator("Database"));
  view_menu->addSeparator();

  ADD_TOGGLE_NS(view_menu, "Database spawns", _draw_db_spawns);

  {
    // The editing panel. A dock on the right, so the spawn list and the save button sit beside
    // the viewport while you place things -- which is the whole workflow.
    auto spawn_dock (new QDockWidget("Database Spawns", _main_window));
    _db_spawn_panel = new Noggit::Ui::DatabaseSpawnPanel(this, spawn_dock);
    spawn_dock->setWidget(_db_spawn_panel);
    spawn_dock->setFeatures
      (QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable
      | QDockWidget::DockWidgetClosable);
    _main_window->addDockWidget(Qt::RightDockWidgetArea, spawn_dock);
    spawn_dock->hide();

    connect(this, &QObject::destroyed, spawn_dock, &QObject::deleteLater);

    auto show_panel (new QAction("Database spawn editor", this));
    show_panel->setCheckable(true);
    view_menu->addAction(show_panel);

    connect(show_panel, &QAction::toggled, spawn_dock, &QWidget::setVisible);
    connect(spawn_dock, &QDockWidget::visibilityChanged, show_panel, &QAction::setChecked);
  }

  {
    // Missing objects. Same dock shape as the spawn panel above, on the right, hidden until
    // asked for -- a healthy map never needs it and it should not take space from the viewport
    // to say so.
    //
    // The panel polls MissingPlacementLog while it is visible; nothing pushes into it from a
    // loader thread. See MissingObjectsPanel.hpp for why that direction was chosen.
    auto missing_dock (new QDockWidget("Missing Objects", _main_window));
    _missing_objects_panel
      = new Noggit::Ui::Tools::MissingObjects::MissingObjectsPanel(this, missing_dock);
    missing_dock->setWidget(_missing_objects_panel);
    missing_dock->setFeatures
      (QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable
      | QDockWidget::DockWidgetClosable);
    _main_window->addDockWidget(Qt::RightDockWidgetArea, missing_dock);
    missing_dock->hide();

    connect(this, &QObject::destroyed, missing_dock, &QObject::deleteLater);

    _missing_objects_dock = missing_dock;

    auto show_missing (new QAction("Missing objects", this));
    show_missing->setCheckable(true);
    show_missing->setStatusTip
      ("Every placement whose model or WMO failed to load this session, with its UID, position "
       "and tile.");
    view_menu->addAction(show_missing);

    connect(show_missing, &QAction::toggled, missing_dock, &QWidget::setVisible);
    connect(missing_dock, &QDockWidget::visibilityChanged, show_missing, &QAction::setChecked);
  }

  {
    // Two actions rather than one, because the cheap case is the common one. Loading the tile
    // under the camera is what the edit workflow needs and costs two queries; loading everything
    // streamed in is occasionally useful and, against a populated world database, expensive
    // enough to be worth asking for deliberately.
    auto load_tile (new QAction("Load spawns (this tile)", this));
    load_tile->setStatusTip
      ("Read creature and gameobject spawns for the tile under the camera. Read-only; emits "
       "nothing.");
    view_menu->addAction(load_tile);
    connect(load_tile, &QAction::triggered, this, [this] { loadDatabaseSpawns(false); });

    auto load_all (new QAction("Load spawns (all loaded tiles)...", this));
    load_all->setStatusTip
      ("Read spawns for every loaded tile. Counts them first and asks before loading a large "
       "set. Read-only; emits nothing.");
    view_menu->addAction(load_all);
    connect(load_all, &QAction::triggered, this, [this] { loadDatabaseSpawns(true); });
  }

  auto debug_menu (view_menu->addMenu ("Debug"));
  ADD_TOGGLE_NS (debug_menu, "Occlusion boxes", _draw_occlusion_boxes);

  view_menu->addSeparator();
  view_menu->addAction(createTextSeparator("Tools"));
  view_menu->addSeparator();

  ADD_TOGGLE (view_menu, "Show Node Editor", "Shift+N", _show_node_editor);

  // ADD_TOGGLE_NS(view_menu, "Game View", _game_mode_camera);

  view_menu->addSeparator();
  view_menu->addAction(createTextSeparator("Minimap"));
  view_menu->addSeparator();

  ADD_TOGGLE (view_menu, "Show", Qt::Key_M, _show_minimap_window);


  ADD_TOGGLE_NS(view_menu, "Show ADT borders", _show_minimap_borders);

  ADD_TOGGLE_NS(view_menu, "Show light zones", _show_minimap_skies);

  view_menu->addSeparator();
  view_menu->addAction(createTextSeparator("Windows"));
  view_menu->addSeparator();

  auto hide_widgets = [=]
  {

    QWidget *widget_list[] =
      {
        _detail_infos_dock,
        _keybindings,
        _minimap_dock,
        _asset_browser_dock,
        _overlay_widget,
        _tool_panel_dock

      };

    if (_main_window->displayed_widgets.empty())
    {
      for (auto widget : widget_list)
        if (widget->isVisible())
        {
          _main_window->displayed_widgets.emplace(widget);
          widget->hide();
        }

    }
    else
    {
      for (auto widget : _main_window->displayed_widgets)
        widget->show();

      _main_window->displayed_widgets.clear();
    }


    _main_window->statusBar()->setVisible(ui_hidden);
    _toolbar->setVisible(ui_hidden);
    _view_toolbar->setVisible(ui_hidden);

    ui_hidden = !ui_hidden;

    setToolPropertyWidgetVisibility(terrainMode);

  };

  ADD_ACTION(view_menu, "Toggle UI", Qt::Key_Tab, hide_widgets);

  ADD_TOGGLE (view_menu, "Detail infos", Qt::Key_F8, _show_detail_info_window);

  addHotkey( Qt::Key_H
    , MOD_none
    , [this] { activeTool()->onHotkeyPress("toggleTexturePalette"_hash); }
    , [this] { return activeTool()->hotkeyCondition("toggleTexturePalette"_hash); }
  );

  ADD_ACTION (view_menu, "Increase time speed", Qt::Key_N, [this] { mTimespeed += 90.0f; });
  ADD_ACTION (view_menu, "Decrease time speed", Qt::Key_B, [this] { mTimespeed = std::max (0.0f, mTimespeed - 90.0f); });
  ADD_ACTION (view_menu, "Pause time", Qt::Key_J, [this] { mTimespeed = 0.0f; });
  ADD_ACTION (view_menu, "Invert mouse", "I", [this] { mousedir *= -1.f; });
  ADD_ACTION (view_menu, "Decrease camera speed", Qt::Key_O, [this] { _camera.move_speed *= 0.5f; });
  ADD_ACTION (view_menu, "Increase camera speed", Qt::Key_P, [this] { _camera.move_speed *= 2.0f; });
  ADD_ACTION ( view_menu
  , "Turn camera around 180°"
  , "Shift+R"
  , [this]
               {
                 _camera.add_to_yaw(math::degrees(180.f));
                 _camera_moved_since_last_draw = true;
               }
  );

  ADD_ACTION ( view_menu
  , "Toggle tile mode"
  , Qt::Key_U
  , [this]
               {
                 if (NOGGIT_CUR_ACTION)
                   return;

                 if (_display_mode == display_mode::in_2D)
                 {
                   _display_mode = display_mode::in_3D;
                   set_editing_mode (saveterrainMode);
                 }
                 else
                 {
                   _display_mode = display_mode::in_2D;
                   saveterrainMode = terrainMode;
                   set_editing_mode (editing_mode::paint);
                 }
               }
  );

  view_menu->addSeparator();
  view_menu->addAction(createTextSeparator("Camera Modes"));
  view_menu->addSeparator();

  /* // TODO, doesn't work for some reason.
  ADD_TOGGLE_NS(view_menu, "Debug cam", _debug_cam_mode);
  connect(&_debug_cam_mode, &Noggit::BoolToggleProperty::changed
      , [this]
      {
          _debug_cam = Noggit::Camera(_camera.position, _camera.yaw(), _camera.pitch());
      }
  );

  ADD_ACTION_NS(view_menu
      , "Go to debug camera"
      , [this]
      {
          _camera = Noggit::Camera(_debug_cam.position, _debug_cam.yaw(), _debug_cam.pitch());
      }
  );*/

  ADD_TOGGLE_NS(view_menu, "FPS camera", _fps_mode);
  connect(&_fps_mode, &Noggit::BoolToggleProperty::changed
    , [this]
    {
      setCameraDirty();
      auto ground_pos = getWorld()->get_ground_height(getCamera()->position);
      getCamera()->position.y = ground_pos.y + 2;
    }
  );

  ADD_TOGGLE_NS(view_menu, "Camera Collision", _camera_collision);

}

void MapView::setupToolsMenu()
{
    auto menu(_main_window->_menuBar->addMenu("Tools"));
    connect(this, &QObject::destroyed, menu, &QObject::deleteLater);

    for (auto&& tool : _tools)
    {
        tool->registerMenuItems(menu);
    }

    // Ground effect set authoring. A dialog rather than a dock, because it is a library editor
    // used occasionally rather than a brush used continuously -- and because it deliberately does
    // not belong inside the Texturing tool, where the existing GroundEffectsTool lives.
    menu->addSeparator();

    auto ground_effect_sets (new QAction("Ground Effect Sets...", this));
    ground_effect_sets->setStatusTip
      ("Create and edit ground effect sets, and apply one to every chunk using a texture.");
    menu->addAction(ground_effect_sets);

    connect(ground_effect_sets, &QAction::triggered, this, [this]
      {
        // Built on demand and owned by this view, so the DBC state it reads is whatever is
        // currently loaded rather than a snapshot from startup.
        auto editor (new Noggit::Ui::GroundEffectSetEditor(this, this));
        editor->setAttribute(Qt::WA_DeleteOnClose);
        editor->show();
      }
    );

    // The three entries below share the separator opened above rather than adding their own.
    // They are all the same species as the ground effect editor -- a modeless dialog with its own
    // scope selector, doing a bulk pass over loaded tiles -- and splitting them with more rules
    // would suggest they were unrelated groups. Each is built on demand and owned by this view, so
    // the terrain and textures it offers are whatever is loaded now, not a snapshot from startup.
    //
    // Assist was the alternative home and is wrong for all three: every Assist entry acts on the
    // ADT under the camera the instant it is clicked, with no UI and no result reported, and the
    // point of these is that they state what they examined before anything is written.

    // Rule-driven automatic texturing. A dialog rather than part of the Texturing tool's dock
    // because it is a bulk operation over loaded tiles, not a brush.
    auto auto_texture (new QAction("Automatic Texturing...", this));
    auto_texture->setStatusTip
      ("Texture terrain from slope and height rules, with a coverage preview before anything is "
       "painted.");
    menu->addAction(auto_texture);

    connect(auto_texture, &QAction::triggered, this, [this]
      {
        auto dialog (new Noggit::Ui::AutoTextureDialog(this, this));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
      }
    );

    // Alpha map integrity. Reports before it repairs, which is why it needs a window at all.
    auto alpha_integrity (new QAction("Alpha Map Integrity...", this));
    alpha_integrity->setStatusTip
      ("Find and repair alpha map states an ADT can store but no renderer can display.");
    menu->addAction(alpha_integrity);

    connect(alpha_integrity, &QAction::triggered, this, [this]
      {
        auto report (new Noggit::Ui::AlphaIntegrityReport(this, this));
        report->setAttribute(Qt::WA_DeleteOnClose);
        report->show();
      }
    );

    // Ambient occlusion bake. Not a Tool: editing_mode::mccv already belongs to VertexPainterTool
    // and ShaderTool, and this is a one-shot bake with a parameter sheet, not a third way to paint.
    auto ambient_occlusion (new QAction("Bake Ambient Occlusion...", this));
    ambient_occlusion->setStatusTip
      ("Bake horizon-sampled ambient occlusion into terrain vertex colour (MCCV).");
    menu->addAction(ambient_occlusion);

    connect(ambient_occlusion, &QAction::triggered, this, [this]
      {
        auto dialog (new Noggit::Ui::AmbientOcclusionDialog(this, this));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
      }
    );
}

void MapView::setupHelpMenu()
{
  auto help_menu (_main_window->_menuBar->addMenu ("Help"));
  connect (this, &QObject::destroyed, help_menu, &QObject::deleteLater);

  ADD_TOGGLE (help_menu, "Key Bindings", "Ctrl+F1", _show_keybindings_window);

#if defined(_WIN32) || defined(WIN32)
  ADD_ACTION_NS ( help_menu
                , "WoW Modding Discord"
                , []
                  {
                    ShellExecute ( nullptr
                                 , "open"
                                 , "https://discord.gg/Dnrztg7dCZ"
                                 , nullptr
                                 , nullptr
                                 , SW_SHOWNORMAL
                                 );
                  }
                );
  ADD_ACTION_NS ( help_menu
                , "Noggit Red Repository"
                , []
                  {
                    ShellExecute ( nullptr
                                 , "open"
                                 , "https://gitlab.com/prophecy-rp/noggit-red/"
                                 , nullptr
                                 , nullptr
                                 , SW_SHOWNORMAL
                                 );
                  }
                );

  ADD_ACTION_NS ( help_menu
                , "Noggit Red Discord"
                , []
                  {
                    ShellExecute ( nullptr
                                 , "open"
                                 , "https://discord.gg/Tk2TpN8CaF"
                                 , nullptr
                                 , nullptr
                                 , SW_SHOWNORMAL
                                 );
                  }
                );
#endif

}

void MapView::setupClientMenu()
{
  // can add this to main menu instead in NoggitWindow()

  auto client_menu(_main_window->_menuBar->addMenu("Client"));
  connect(this, &QObject::destroyed, client_menu, &QObject::deleteLater); // to remove from main menu

  // ADD_ACTION_NS(client_menu, "Start Client",  [this] { _main_window->startWowClient(); });
  auto start_client_action(client_menu->addAction("Start Client"));
  connect(start_client_action, &QAction::triggered, [this] { _main_window->startWowClient(); });

  // ADD_ACTION_NS(client_menu, "Patch Client", [this] { _main_window->patchWowClient(); });
  auto pack_client_action(client_menu->addAction("Patch Client"));
  pack_client_action->setToolTip("Save content of project folder as MPQ patch in the client.");
  connect(pack_client_action, &QAction::triggered, [this] { _main_window->patchWowClient(); });

}

void MapView::setupHotkeys()
{

  addHotkey ( Qt::Key_F1
    , MOD_shift
    , [this]
              {
                if (alloff)
                {
                  alloff_models = _draw_models.get();
                  alloff_doodads = _draw_wmo_doodads.get();
                  alloff_contour = _draw_contour.get();
                  alloff_climb = _draw_climb.get();
                  alloff_vertex_color = _draw_vertex_color.get();
                  alloff_baked_shadows = _draw_baked_shadows.get();
                  alloff_wmo = _draw_wmo.get();
                  alloff_fog = _draw_fog.get();
                  alloff_terrain = _draw_terrain.get();

                  _draw_models.set (false);
                  _draw_wmo_doodads.set (false);
                  _draw_contour.set (true);
                  _draw_climb.set (false);
                  _draw_vertex_color.set(true);
                  _draw_baked_shadows.set(false);
                  _draw_wmo.set (false);
                  _draw_terrain.set (true);
                  _draw_fog.set (false);
                }
                else
                {
                  _draw_models.set (alloff_models);
                  _draw_wmo_doodads.set (alloff_doodads);
                  _draw_contour.set (alloff_contour);
                  _draw_climb.set(alloff_climb);
                  _draw_vertex_color.set(alloff_vertex_color);
                  _draw_baked_shadows.set(alloff_baked_shadows);
                  _draw_wmo.set (alloff_wmo);
                  _draw_terrain.set (alloff_terrain);
                  _draw_fog.set (alloff_fog);
                }
                alloff = !alloff;
              }
  );

  addHotkey(Qt::Key_C, MOD_ctrl, "copySelection"_hash);

  addHotkey(Qt::Key_V, MOD_ctrl, "paste"_hash);

  addHotkey(Qt::Key_V, MOD_shift, "importM2FromWmv"_hash);

  addHotkey(Qt::Key_V, MOD_alt, "importWmoFromWmv"_hash);

  addHotkey(Qt::Key_C, MOD_none, "clearVertexSelection"_hash);

  addHotkey(Qt::Key_B, MOD_ctrl, "duplacteSelection"_hash);

  addHotkey(Qt::Key_Y, MOD_none, "nextType"_hash);

  addHotkey(Qt::Key_T, MOD_none, "toggleAngle"_hash);

  addHotkey(Qt::Key_T, MOD_space, "nextMode"_hash);

  addHotkey(Qt::Key_T, MOD_none, "toggleTool"_hash);

  addHotkey(Qt::Key_T, MOD_none, "unsetAdtHole"_hash);
  addHotkey(Qt::Key_T, MOD_alt, "setAdtHole"_hash);

  addHotkey(Qt::Key_T, MOD_none, "toggleAngled"_hash);

  addHotkey(Qt::Key_T, MOD_none, "togglePasteMode"_hash);

  addHotkey ( Qt::Key_H
    , MOD_none
    , [&]
              {
                if (_world->has_selection())
                {
                  for (auto& selection : _world->current_selection())
                  {
                    if (selection.index() != eEntry_Object)
                      continue;

                    auto obj = std::get<selected_object_type>(selection);

                    if (obj->which() == eMODEL)
                    {
                      static_cast<ModelInstance*>(obj)->model->toggle_visibility();
                    }
                    else if (obj->which() == eWMO)
                    {
                      static_cast<WMOInstance*>(obj)->wmo->toggle_visibility();
                    }
                  }
                }
              }
    , [&] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );

  addHotkey ( Qt::Key_H
    , MOD_space
    , [&]
              {
                _draw_hidden_models.toggle();
              }
    , [&] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );

  addHotkey(Qt::Key_R, MOD_space, "setBrushLevelMinMax"_hash);

  addHotkey ( Qt::Key_H
    , MOD_shift
    , [&]
              {
                ModelManager::clear_hidden_models();
                WMOManager::clear_hidden_wmos();
              }
    , [&] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );

  addHotkey(Qt::Key_F, MOD_space, "toggleLock"_hash);

  addHotkey(Qt::Key_F, MOD_none, "lockCursor"_hash);

  addHotkey ( Qt::Key_F
    , MOD_none
    , [&]
              {

                NOGGIT_ACTION_MGR->beginAction(this, Noggit::ActionFlags::eOBJECTS_TRANSFORMED);
                _world->set_selected_models_pos(_cursor_pos);
                emit rotationChanged();
                NOGGIT_ACTION_MGR->endAction();
              }
    , [&] { return terrainMode == editing_mode::object && !NOGGIT_CUR_ACTION; }
  );

  addHotkey(Qt::Key_Plus, MOD_alt, "increaseRadius"_hash);
  addHotkey(Qt::Key_Minus, MOD_alt, "decreaseRadius"_hash);

  addHotkey (Qt::Key_1, MOD_shift, [this] { _camera.move_speed = 15.0f; });
  addHotkey (Qt::Key_2, MOD_shift, [this] { _camera.move_speed = 50.0f; });
  addHotkey (Qt::Key_3, MOD_shift, [this] { _camera.move_speed = 200.0f; });
  addHotkey (Qt::Key_4, MOD_shift, [this] { _camera.move_speed = 800.0f; });

  addHotkey(Qt::Key_1, MOD_alt, "setBrushLevel0Pct"_hash);
  addHotkey(Qt::Key_2, MOD_alt, "setBrushLevel25Pct"_hash);
  addHotkey(Qt::Key_3, MOD_alt, "setBrushLevel50Pct"_hash);
  addHotkey(Qt::Key_4, MOD_alt, "setBrushLevel75Pct"_hash);
  addHotkey(Qt::Key_5, MOD_alt, "setBrushLevel100Pct"_hash);

  addHotkey(Qt::Key_1, MOD_none, [this] { set_editing_mode(editing_mode::ground); }
    , [this] { return !_mod_num_down && !NOGGIT_CUR_ACTION;  });
  addHotkey (Qt::Key_2, MOD_none, [this] { set_editing_mode (editing_mode::flatten_blur); }
    , [this] { return !_mod_num_down && !NOGGIT_CUR_ACTION;  });
  addHotkey (Qt::Key_3, MOD_none, [this] { set_editing_mode (editing_mode::paint); }
    , [this] { return !_mod_num_down && !NOGGIT_CUR_ACTION;  });
  addHotkey (Qt::Key_4, MOD_none, [this] { set_editing_mode (editing_mode::holes); }
    , [this] { return !_mod_num_down && !NOGGIT_CUR_ACTION;  });
  addHotkey (Qt::Key_5, MOD_none, [this] { set_editing_mode (editing_mode::areaid); }
    , [this] { return !_mod_num_down && !NOGGIT_CUR_ACTION;  });
  addHotkey (Qt::Key_6, MOD_none, [this] { set_editing_mode (editing_mode::impass); }
    , [this] { return !_mod_num_down && !NOGGIT_CUR_ACTION;  });
  addHotkey (Qt::Key_7, MOD_none, [this] { set_editing_mode (editing_mode::water); }
    , [this] { return !_mod_num_down && !NOGGIT_CUR_ACTION;  });
  addHotkey (Qt::Key_8, MOD_none, [this] { set_editing_mode (editing_mode::mccv); }
    , [this] { return !_mod_num_down && !NOGGIT_CUR_ACTION;  });
  addHotkey (Qt::Key_9, MOD_none, [this] { set_editing_mode (editing_mode::object); }
    , [this] { return !_mod_num_down && !NOGGIT_CUR_ACTION;  });

  addHotkey(Qt::Key_0, MOD_ctrl, [this] { change_selected_wmo_doodadset(0); });
  addHotkey(Qt::Key_1, MOD_ctrl, [this] { change_selected_wmo_doodadset(1); });
  addHotkey(Qt::Key_2, MOD_ctrl, [this] { change_selected_wmo_doodadset(2); });
  addHotkey(Qt::Key_3, MOD_ctrl, [this] { change_selected_wmo_doodadset(3); });
  addHotkey(Qt::Key_4, MOD_ctrl, [this] { change_selected_wmo_doodadset(4); });
  addHotkey(Qt::Key_5, MOD_ctrl, [this] { change_selected_wmo_doodadset(5); });
  addHotkey(Qt::Key_6, MOD_ctrl, [this] { change_selected_wmo_doodadset(6); });
  addHotkey(Qt::Key_7, MOD_ctrl, [this] { change_selected_wmo_doodadset(7); });
  addHotkey(Qt::Key_8, MOD_ctrl, [this] { change_selected_wmo_doodadset(8); });
  addHotkey(Qt::Key_9, MOD_ctrl, [this] { change_selected_wmo_doodadset(9); });

  addHotkey(Qt::Key_Escape, MOD_none, [this] { _main_window->close(); });

  addHotkey(Qt::Key_Plus, MOD_none, "addColor"_hash);

  addHotkey(Qt::Key_2, MOD_num, "moveSelectedDown"_hash);
  addHotkey(Qt::Key_8, MOD_num, "moveSelectedUp"_hash);
  addHotkey(Qt::Key_4, MOD_num, "moveSelectedLeft"_hash);
  addHotkey(Qt::Key_6, MOD_num, "moveSelectedRight"_hash);

  addHotkey(Qt::Key_3, MOD_num, "rotateSelectedPitchCcw"_hash);
  addHotkey(Qt::Key_1, MOD_num, "rotateSelectedPitchCw"_hash);

  addHotkey(Qt::Key_7, MOD_num, "rotateSelectedYawCcw"_hash);
  addHotkey(Qt::Key_9, MOD_num, "rotateSelectedYawCw"_hash);

  addHotkey(Qt::Key_Plus, MOD_num, "increaseSelectedScale"_hash);
  addHotkey(Qt::Key_Minus, MOD_num, "decreaseSelectedScale"_hash);

  addHotkey(Qt::Key_F, MOD_none, "setAreaId"_hash);

  addHotkey(Qt::Key_Delete, MOD_none, "deleteSelection"_hash);

  // THE CHUNK MANIPULATOR'S KEYS.
  //
  // Registered LAST on purpose. hotkeys is a std::forward_list and addHotkey emplaces at the
  // FRONT (MapView.cpp:6371), while the dispatch loop returns on the first entry whose key,
  // modifiers and condition all match (:5646-5656). So a key registered here is tested before
  // the earlier claim on the same key, and reaches it only when ChunkTool's condition -- chunk
  // editing mode, no action open -- is false. That is what lets C keep clearing the vertex
  // selection, F keep setting an area id and locking the cursor, and R keep setting the brush
  // level everywhere except in this one mode.
  //
  // V, X and Alt+F were unclaimed before this; C, F and R were not.
  addHotkey(Qt::Key_C, MOD_none, "chunkCopy"_hash);
  addHotkey(Qt::Key_V, MOD_none, "chunkPaste"_hash);
  addHotkey(Qt::Key_X, MOD_none, "chunkClearSelection"_hash);
  addHotkey(Qt::Key_R, MOD_none, "chunkRotate90"_hash);
  addHotkey(Qt::Key_F, MOD_none, "chunkMirrorHorizontal"_hash);
  addHotkey(Qt::Key_F, MOD_alt, "chunkMirrorVertical"_hash);
}

void MapView::setupMinimap()
{
  _minimap = new Noggit::Ui::minimap_widget(this);
  _minimap_dock = new QDockWidget("Minimap", this);
  _minimap_dock->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
  _minimap_dock->setFixedSize(_minimap->sizeHint());
  _minimap_dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);

  _minimap->world (_world.get());
  _minimap->camera (&_camera);
  _minimap->draw_boundaries (_show_minimap_borders.get());
  _minimap->draw_skies (_show_minimap_skies.get());
  _minimap->set_resizeable(true);

  connect ( _minimap, &Noggit::Ui::minimap_widget::map_clicked
    , [this] (glm::vec3 const& pos)
            {
              move_camera_with_auto_height (pos);
            }
  );

  _minimap_dock->setFeatures ( QDockWidget::DockWidgetMovable
                               | QDockWidget::DockWidgetFloatable
                               | QDockWidget::DockWidgetClosable
  );
  auto minimap_scroll_area = new QScrollArea(_minimap_dock);
  minimap_scroll_area->setWidget(_minimap);
  minimap_scroll_area->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);

  _minimap_dock->setWidget(minimap_scroll_area);
  _main_window->addDockWidget (Qt::LeftDockWidgetArea, _minimap_dock);
  _minimap_dock->setVisible (false);
  _minimap_dock->setFloating(true);
  _minimap_dock->move(_main_window->rect().center() - _minimap->rect().center());


  connect(this, &QObject::destroyed, _minimap_dock, &QObject::deleteLater);
  connect(this, &QObject::destroyed, _minimap, &QObject::deleteLater);

  connect ( &_show_minimap_window, &Noggit::BoolToggleProperty::changed
    , _minimap_dock, [this]
            {
              if (!ui_hidden)
                _minimap_dock->setVisible(_show_minimap_window.get());
            }
  );


  connect ( _minimap_dock, &QDockWidget::visibilityChanged
    , &_show_minimap_window, &Noggit::BoolToggleProperty::set
  );

  connect ( &_show_minimap_borders, &Noggit::BoolToggleProperty::changed
    , [this]
            {
              _minimap->draw_boundaries(_show_minimap_borders.get());
            }
  );

  connect ( &_show_minimap_skies, &Noggit::BoolToggleProperty::changed
    , [this]
            {
              _minimap->draw_skies(_show_minimap_skies.get());
            }
  );

}

void MapView::createGUI()
{
  // Combined dock
  _tool_panel_dock = new Noggit::Ui::Tools::ToolPanel(this);
  _tool_panel_dock->setFeatures(QDockWidget::DockWidgetMovable
                                | QDockWidget::DockWidgetFloatable);
  _tool_panel_dock->setAllowedAreas(Qt::RightDockWidgetArea | Qt::LeftDockWidgetArea);

  connect(this, &QObject::destroyed, _tool_panel_dock, &QObject::deleteLater);
  _main_window->addDockWidget(Qt::RightDockWidgetArea, _tool_panel_dock);

  setupAssetBrowser();

  _tools.emplace_back(std::make_unique<Noggit::RaiseLowerTool>(this))->setupUi(_tool_panel_dock);
  _tools.emplace_back(std::make_unique<Noggit::FlattenBlurTool>(this))->setupUi(_tool_panel_dock);
  _tools.emplace_back(std::make_unique<Noggit::TexturingTool>(this))->setupUi(_tool_panel_dock);
  _tools.emplace_back(std::make_unique<Noggit::HoleTool>(this))->setupUi(_tool_panel_dock);
  _tools.emplace_back(std::make_unique<Noggit::AreaTool>(this))->setupUi(_tool_panel_dock);
  _tools.emplace_back(std::make_unique<Noggit::ImpassTool>(this))->setupUi(_tool_panel_dock);
  _tools.emplace_back(std::make_unique<Noggit::WaterTool>(this))->setupUi(_tool_panel_dock);
  _tools.emplace_back(std::make_unique<Noggit::VertexPainterTool>(this))->setupUi(_tool_panel_dock);
  _tools.emplace_back(std::make_unique<Noggit::ObjectTool>(this))->setupUi(_tool_panel_dock);
  _tools.emplace_back(std::make_unique<Noggit::MinimapTool>(this))->setupUi(_tool_panel_dock);
  _tools.emplace_back(std::make_unique<Noggit::StampTool>(this))->setupUi(_tool_panel_dock);
  _tools.emplace_back(std::make_unique<Noggit::LightTool>(this))->setupUi(_tool_panel_dock);
  _tools.emplace_back(std::make_unique<Noggit::ScriptingTool>(this))->setupUi(_tool_panel_dock);
  _tools.emplace_back(std::make_unique<Noggit::ChunkTool>(this))->setupUi(_tool_panel_dock);
  _tools.emplace_back(std::make_unique<Noggit::AreaTriggerTool>(this))->setupUi(_tool_panel_dock);
  // Position in this sequence IS the editing_mode value -- MapView indexes _tools with the
  // enumerator directly (selectedTexturePath, and the paint/object resets in the destructor).
  // Erosion is editing_mode::erosion = 15, so it is appended here and nothing above it moves.
  _tools.emplace_back(std::make_unique<Noggit::ErosionTool>(this))->setupUi(_tool_panel_dock);

  // End combined dock

  setupViewportOverlay();
  // texturingTool->setup_ge_tool_renderer();
  setupNodeEditor();
  setupDetailInfos();
  setupToolbars();
  setupKeybindingsGui();

  setupMinimap();
  setupFileMenu();
  setupEditMenu();
  setupViewMenu();
  setupToolsMenu();
  setupAssistMenu();
  setupHelpMenu();
  setupClientMenu();
  setupHotkeys();

  setupMainToolbar();

  for (auto&& tool : _tools)
  {
      tool->postUiSetup();
  }

  connect(_main_window, &Noggit::Ui::Windows::NoggitWindow::exitPromptOpened, this, &MapView::on_exit_prompt);

  set_editing_mode (editing_mode::ground);

  // do we need to do this every tick ?
#ifdef USE_MYSQL_UID_STORAGE
  if (_settings->value("project/mysql/enabled").toBool())
  {
      if (mysql::hasMaxUIDStoredDB(_world->getMapID()))
      {
        _status_database->setText("MySQL UID sync enabled: "
            + _settings->value("project/mysql/server").toString() + ":"
            + _settings->value("project/mysql/port").toString());
      }
  }
#endif
}

void MapView::on_exit_prompt()
{
  // hide all popups
  _keybindings->hide();
  _minimap_dock->hide();
  _detail_infos_dock->hide();
}

MapView::MapView( math::degrees camera_yaw0
                , math::degrees camera_pitch0
                , glm::vec3 camera_pos
                , Noggit::Ui::Windows::NoggitWindow* NoggitWindow
				        , std::shared_ptr<Noggit::Project::NoggitProject> Project
                , std::unique_ptr<World> world
                , uid_fix_mode uid_fix
                , bool from_bookmark
                )
  : _camera (camera_pos, camera_yaw0, camera_pitch0)
  , mTimespeed(0.0f)
  , _uid_fix (uid_fix)
  , _from_bookmark (from_bookmark)
  , _settings (new QSettings (this))
  , cursor_color (1.f, 1.f, 1.f, 1.f)
  , _cursorType{CursorType::CIRCLE}
  , _main_window (NoggitWindow)
  , _debug_cam(camera_pos, camera_yaw0, camera_pitch0)
  , _world (std::move (world))
  , _status_position (new QLabel (this))
  , _status_selection (new QLabel (this))
  , _status_area (new QLabel (this))
  , _status_time (new QLabel (this))
  , _status_fps (new QLabel (this))
  , _status_culling (new QLabel (this))
  , _status_database(new QLabel(this))
  , _texBrush{new OpenGL::texture{}}
  , _transform_gizmo(Noggit::Ui::Tools::ViewportGizmo::GizmoContext::MAP_VIEW)
  , _tablet_manager(Noggit::TabletManager::instance()),
    _project(Project)
{
  setWindowTitle ("Noggit Crimson");
  setFocusPolicy (Qt::StrongFocus);
  setMouseTracking (true);

  setMinimumHeight(200);
  setMaximumHeight(10000);
  setAttribute(Qt::WA_OpaquePaintEvent, true);
  setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);

  _world->LoadSavedSelectionGroups(); // not doing this in world constructor because noggit loads world twice

  _context = Noggit::NoggitRenderContext::MAP_VIEW;
  _transform_gizmo.setWorld(_world.get());

  _main_window->setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
  _main_window->setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
  _main_window->setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
  _main_window->setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);

  _main_window->statusBar()->addWidget (_status_position);
  connect ( this
          , &QObject::destroyed
          , _main_window
          , [=] { _main_window->statusBar()->removeWidget (_status_position); }
          );
  _main_window->statusBar()->addWidget (_status_selection);
  connect ( this
          , &QObject::destroyed
          , _main_window
          , [=] { _main_window->statusBar()->removeWidget (_status_selection); }
          );
  _main_window->statusBar()->addWidget (_status_area);
  connect ( this
          , &QObject::destroyed
          , _main_window
          , [=] { _main_window->statusBar()->removeWidget (_status_area); }
          );
  _main_window->statusBar()->addWidget (_status_time);
  connect ( this
          , &QObject::destroyed
          , _main_window
          , [=] { _main_window->statusBar()->removeWidget (_status_time); }
          );
  _main_window->statusBar()->addWidget (_status_fps);
  connect ( this
          , &QObject::destroyed
          , _main_window
          , [=] { _main_window->statusBar()->removeWidget (_status_fps); }
          );
  _main_window->statusBar()->addWidget (_status_culling);
  connect ( this
      , &QObject::destroyed
      , _main_window
      , [=] { _main_window->statusBar()->removeWidget (_status_culling); }
  );
  _main_window->statusBar()->addWidget(_status_database);
  connect(this
      , &QObject::destroyed
      , _main_window
      , [=] { _main_window->statusBar()->removeWidget(_status_database); }
  );

  setContextMenuPolicy(Qt::CustomContextMenu);

  connect(this, SIGNAL(customContextMenuRequested(const QPoint&)),
      this, SLOT(ShowContextMenu(const QPoint&)));

  connect(this, &MapView::selectionUpdated, [this](std::vector<selection_type>&)
      {
          // updateDetailInfos();
      });

  moving = strafing = updown = lookat = turn = 0.0f;

  freelook = false;

  mousedir = -1.0f;

  look = false;
  _display_mode = display_mode::in_3D;

  _startup_time.start();

  // The default was 60, and measurement showed that is where nearly all the frame budget went:
  // 2.5 ms of work inside paintGL against a 15.9 ms frame, so 84% of every frame was spent
  // waiting on this timer. No renderer optimisation can be seen while that holds -- halving the
  // work produces an identical frame interval. 144 is a common high-refresh rate and remains a
  // cap rather than a target; the setting is unchanged and anyone who wants 60 can set it.
  // One-time migration off the old 60 default. Raising the default alone would have changed
  // nothing for anyone who has ever opened this build before, because QSettings already holds
  // their 60 -- and "the default is now 144" reads like a fix while doing literally nothing.
  //
  // Keyed on a marker rather than on the value, so this runs exactly once. Someone who genuinely
  // wants 60 sets it after the migration and it stays; the marker is already written by then.
  if (!_settings->value("fps_limit_default_migrated", false).toBool())
  {
    if (_settings->value("fps_limit", 60).toInt() == 60)
    {
      _settings->setValue("fps_limit", 144);
    }

    _settings->setValue("fps_limit_default_migrated", true);
  }

  int _fps_limit = _settings->value("fps_limit", 144).toInt();

  // A stored 0 or a negative value used to divide by zero and then convert a non-finite float to
  // int, which is undefined behaviour. Clamped rather than trusted.
  _fps_limit = std::clamp(_fps_limit, 1, 1000);

  int _frametime = std::max(1, static_cast<int>(1000.f / static_cast<float>(_fps_limit)));
  std::cout << "FPS limit is set to : " << _fps_limit << " (" << _frametime << ")" << std::endl;

  // PreciseTimer, because Qt's default CoarseTimer allows 5% drift -- at these intervals that is
  // most of a frame, and it is the difference between a steady cadence and visible unevenness.
  _update_every_event_loop.setTimerType (Qt::PreciseTimer);
  _update_every_event_loop.start (_frametime);
  connect(&_update_every_event_loop, &QTimer::timeout,[=]
      { 
          _needs_redraw = true;

          Qt::ApplicationState app_state = QGuiApplication::applicationState();
          if (app_state == Qt::ApplicationState::ApplicationSuspended)
          {
              _needs_redraw = false;
              return;
          };

          if (_main_window->isMinimized() && _settings->value("background_fps_limit", true).toBool())
          {
              _needs_redraw = false;
              // return;
          }

          update();
      });

  // reduce frame rate in background
  connect(QGuiApplication::instance(), SIGNAL(applicationStateChanged(Qt::ApplicationState)),
      this, SLOT(onApplicationStateChanged(Qt::ApplicationState)));

  createGUI();
}

void MapView::tabletEvent(QTabletEvent* event)
{
  _tablet_manager->setPressure(event->pressure());
  _tablet_manager->setIsActive(true);
  event->ignore();
}

auto MapView::setBrushTexture(QImage const* img) -> void
{

  int const height{img->height()};
  int const width{img->width()};

  std::vector<std::uint32_t> tex(height * width);

  for(int i{}; i < height; ++i)
    for(int j{}; j < width; ++j)
      tex[i * width + j] = img->pixel(j, i);

  makeCurrent();
  OpenGL::context::scoped_setter const _{gl, context()};
  OpenGL::texture::set_active_texture(4);
  _texBrush->bind();
  gl.texImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex.data());
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

Noggit::Camera* MapView::getCamera()
{
  return &_camera;
}

void MapView::move_camera_with_auto_height (glm::vec3 const& pos)
{
  makeCurrent();
  OpenGL::context::scoped_setter const _ (::gl, context());

  TileIndex tile_index = TileIndex(pos);
  if (_world->mapIndex.hasTile(tile_index))
  {
    _world->mapIndex.loadTile(pos)->wait_until_loaded();
  }

  _camera.position = pos;
  _camera.position.y = 0.0f;

  _world->GetVertex (pos.x, pos.z, &_camera.position);

  // min elevation according to https://wowdev.wiki/AreaTable.dbc
  //! \ todo use the current area's MinElevation
  if (_camera.position.y < -5000.0f)
  {
    //! \todo use the height of a model/wmo of the tile (or the map) ?
    _camera.position.y = 0.0f;
  }

  _camera.position.y += 50.0f;

  _camera_moved_since_last_draw = true;
}

void MapView::on_uid_fix_fail()
{
  emit uid_fix_failed();

  _uid_fix_failed = true;
  deleteLater();
}

void MapView::initializeGL()
{
  bool uid_warning = false;

  OpenGL::context::scoped_setter const _ (::gl, context());

  gl.viewport(0.0f, 0.0f, width(), height());

  gl.clearColor (0.0f, 0.0f, 0.0f, 1.0f);

  if (_uid_fix == uid_fix_mode::max_uid)
  {
    _world->mapIndex.searchMaxUID();
  }
  else if (_uid_fix == uid_fix_mode::fix_all_fail_on_model_loading_error)
  {
    auto result = _world->mapIndex.fixUIDs (_world.get(), true);

    if (result == uid_fix_status::failed)
    {
      on_uid_fix_fail();
      return;
    }
  }
  else if (_uid_fix == uid_fix_mode::fix_all_fuckporting_edition)
  {
    auto result = _world->mapIndex.fixUIDs (_world.get(), false);

    uid_warning = result == uid_fix_status::done_with_errors;
  }

  _uid_fix = uid_fix_mode::none;

  if (!_from_bookmark)
  {
    move_camera_with_auto_height (_camera.position);
  }

  if (uid_warning)
  {
    QMessageBox::warning
      ( nullptr
      , "UID Warning"
      , "Some models were missing or couldn't be loaded. "
        "This will lead to culling (visibility) errors in game\n"
        "It is recommended to fix those models (listed in the log file) and run the uid fix all again."
      , QMessageBox::Ok
      );
  }

  _imgui_context = QtImGui::initialize(this);

  emit resized();

  _last_opengl_context = context();

  _world->renderer()->upload();
  onSettingsSave();

  _buffers.upload();

  gl.bufferData<GL_PIXEL_PACK_BUFFER>(_buffers[0], 4, nullptr, GL_DYNAMIC_READ);
  gl.bufferData<GL_PIXEL_PACK_BUFFER>(_buffers[1], 4, nullptr, GL_DYNAMIC_READ);

  connect(context(), &QOpenGLContext::aboutToBeDestroyed, [this](){ emit aboutToLooseContext(); });

  _gl_initialized = true;
}

void MapView::paintGL()
{
  ZoneScoped;

  static bool lock = false;

  if (lock)
    return;

  if (!_needs_redraw)
  {
    return;
  }

  _needs_redraw = false;

  if (!_gl_initialized)
  {
    initializeGL();
  }

  if (_last_opengl_context != context())
  {
    _gl_initialized = false;
    return;
  }

  const qreal now(_startup_time.elapsed() / 1000.0);

  _last_frame_durations.emplace_back (now - _last_update);

  lock = true;
  if (!activeTool()->preRender())
  {
      lock = false;
      return;
  }
  lock = false;

  OpenGL::context::scoped_setter const _(::gl, context());
  makeCurrent();

  gl.clear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  {
    lock = true;
    draw_map();
    activeTool()->postRender();
    lock = false;
    tick (now - _last_update);
  }

  _last_update = now;

  // Two mutually exclusive gizmo consumers, never both in one frame.
  //
  // The object editor keeps precedence and its branch below is unchanged. The spawn branch exists
  // because a database spawn is not in _world->current_selection() -- it is not a SceneObject in
  // the ADT scene graph at all -- so under the original condition no ImGui frame was begun when
  // only a spawn was selected, and anything drawn would have gone nowhere.
  //
  // spawnGizmoTarget() already returns nothing while the object gizmo has a target; the explicit
  // split here is what guarantees the object path is entered on exactly the frames
  // handleTransformGizmo would have drawn on -- objectGizmoHasTarget() is that function's own
  // early-return condition, not a second opinion about it.
  Noggit::Database::SpawnRef const gizmo_spawn (spawnGizmoTarget());
  int const gizmo_light (lightGizmoTarget());

  bool const object_gizmo_frame = _gizmo_on.get() && objectGizmoHasTarget();
  bool const spawn_gizmo_frame = _gizmo_on.get() && !object_gizmo_frame && gizmo_spawn.valid();
  bool const light_gizmo_frame
    = _gizmo_on.get() && !object_gizmo_frame && !spawn_gizmo_frame && gizmo_light != 0;

  if (object_gizmo_frame || spawn_gizmo_frame || light_gizmo_frame)
  {
    ImGui::SetCurrentContext(_imgui_context);
    QtImGui::newFrame();

    static bool is_open = false;
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::SetNextWindowPos(ImVec2(-100.f, -100.f));
    ImGui::Begin("Gizmo", &is_open, ImGuiWindowFlags_::ImGuiWindowFlags_NoTitleBar
                                                | ImGuiWindowFlags_::ImGuiWindowFlags_NoBackground);

    if (object_gizmo_frame)
    {
    // auto mv = model_view();
    // auto proj = projection();

    _transform_gizmo.setCurrentGizmoOperation(_gizmo_operation);
    _transform_gizmo.setCurrentGizmoMode(_gizmo_mode);
    _transform_gizmo.setUseMultiselectionPivot(activeTool()->useMultiselectionPivot());

    auto pivot = _world->multi_select_pivot().has_value() ?
        _world->multi_select_pivot().value() : glm::vec3(0.f, 0.f, 0.f);

    _transform_gizmo.setMultiselectionPivot(pivot);

    _transform_gizmo.handleTransformGizmo(this, _world->current_selection(), _model_view, _projection);

    // _world->update_selection_pivot();
    // Left inside the object branch on purpose. Tools that draw their own ImGuizmo -- AreaTriggerTool
    // is the one that does -- have only ever run on frames where World had a selection, and letting
    // them run on a spawn-only frame would put a second gizmo in it.
    activeTool()->renderImGui(_gizmo_mode, _gizmo_operation);
    }
    else if (spawn_gizmo_frame)
    {
      handleSpawnGizmo(gizmo_spawn);
    }
    else
    {
      // A light is not in _world->current_selection() either -- it is not a SceneObject and Sky
      // is not in selection_type at all -- so it needs its own branch for exactly the reason the
      // spawn branch above needs one: under the original condition no ImGui frame was begun on a
      // light-only frame, and anything drawn would have gone nowhere.
      handleLightGizmo(gizmo_light);
    }

    ImGui::End();

    /* Example
    std::string sText;

    if(ImGui::IsMouseClicked( 1 ) )
    {
      ImGui::OpenPopup( "PieMenu" );
    }

    if( BeginPiePopup( "PieMenu", 1 ) )
    {
      if( PieMenuItem( "Test1" ) ) sText = "Test1";
      if( PieMenuItem( "Test2" ) )
      {
        sText = "Test2";
      }
      if( PieMenuItem( "Test3", false ) ) sText = "Test3";
      if( BeginPieMenu( "Sub" ) )
      {
        if( BeginPieMenu( "Sub sub\nmenu" ) )
        {
          if( PieMenuItem( "SubSub" ) ) sText = "SubSub";
          if( PieMenuItem( "SubSub2" ) ) sText = "SubSub2";
          EndPieMenu();
        }
        if( PieMenuItem( "TestSub" ) ) sText = "TestSub";
        if( PieMenuItem( "TestSub2" ) ) sText = "TestSub2";
        EndPieMenu();
      }
      if( BeginPieMenu( "Sub2" ) )
      {
        if( PieMenuItem( "TestSub" ) ) sText = "TestSub";
        if( BeginPieMenu( "Sub sub\nmenu" ) )
        {
          if( PieMenuItem( "SubSub" ) ) sText = "SubSub";
          if( PieMenuItem( "SubSub2" ) ) sText = "SubSub2";
          EndPieMenu();
        }
        if( PieMenuItem( "TestSub2" ) ) sText = "TestSub2";
        EndPieMenu();
      }

      EndPiePopup();
    }

   */

    //ImGui::ShowDemoWindow();
    //ImGui::ShowStyleEditor();

    ImGui::Render();

  }

  if (_world->uid_duplicates_found() && !_uid_duplicate_warning_shown)
  {
    _uid_duplicate_warning_shown = true;

    // Deferred out of paintGL rather than raised here, and that is the whole point of the
    // singleShot. QMessageBox::critical is MODAL: it spins a nested event loop, which delivers
    // further paints and timer events while this paintGL call is still on the stack and while the
    // OpenGL context scoped_setter above is still alive. When the dialog closes and that setter
    // unwinds, it runs verify_context_and_check_for_gl_errors against a context the nested loop
    // has since changed -- and OpenGL::Scoped's destructor throws. A throw from a destructor is
    // std::terminate, so the editor died on OK rather than on the error it was reporting.
    //
    // A zero-timer posts to the event loop, so the dialog is shown after paintGL has returned and
    // every GL scope has been destroyed normally. Nothing else about the warning changes.
    //
    // Worth stating because the message misleads: the duplicates are ALREADY REPAIRED by this
    // point. world_model_instances_storage renumbers each colliding object with a fresh uid as it
    // loads it and records the collision. This is a notice that the files on disk carry duplicate
    // uids and should be saved, not a report of an unrecoverable state.
    QTimer::singleShot ( 0
                       , this
                       , [this]
                         {
                           QMessageBox::critical
                             ( this
                             , "UID ALREADY IN USE"
                             , "Objects with duplicate UIDs were found and have been renumbered "
                               "automatically as the map loaded.\n\n"
                               "Save the map to make the fix permanent. Assist > UID collision "
                               "report lists exactly what was renumbered.\n\n"
                               "This normally happens after copying tiles in from another project."
                             );
                         }
                       );
  }

  FrameMark
}

void MapView::resizeGL (int width, int height)
{
  OpenGL::context::scoped_setter const _ (::gl, context());
  gl.viewport(0.0f, 0.0f, width, height);
  emit resized();
  _camera_moved_since_last_draw = true;
  _needs_redraw = true;
}


MapView::~MapView()
{
  makeCurrent();

  _destroying = true;

  _main_window->removeToolBar(_main_window->_app_toolbar);

  OpenGL::context::scoped_setter const _ (::gl, context());

  // Released here, inside the context, rather than left to the member's own destruction after
  // this body returns. By then the context is gone, and every model these instances hold the
  // last reference to would destroy its OpenGL vertex arrays without one -- which throws from a
  // destructor and terminates. Same hazard as the tools below.
  _db_spawn_scene.reset();

  delete _texBrush;
  delete _viewport_overlay_ui;

  // when the uid fix fail the UI isn't created
  if (!_uid_fix_failed)
  {
    // delete TexturePicker; // explicitly delete this here to avoid opengl context related crash
    // delete objectEditor;
    // since the ground effect tool preview renderer got added, this causes crashing on exit to menu. 
    // Now it crashes in application exit.
    // delete texturingTool;

    _tools[static_cast<int>(editing_mode::paint)].reset();
    _tools[static_cast<int>(editing_mode::object)].reset();
  }
  
  if (_force_uid_check)
  {
    uid_storage::remove_uid_for_map(_world->getMapID());
  }

  _world.reset();

  AsyncLoader::instance->reset_object_fail();

  Noggit::Ui::selected_texture::texture.reset();

  ModelManager::report();
  TextureManager::report();
  WMOManager::report();

  NOGGIT_ACTION_MGR->disconnect();

  _buffers.unload();

}

void MapView::tick (float dt)
{
	_mod_shift_down = QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);
	_mod_ctrl_down = QApplication::keyboardModifiers().testFlag(Qt::ControlModifier);
	_mod_alt_down = QApplication::keyboardModifiers().testFlag(Qt::AltModifier);
	_mod_num_down = QApplication::keyboardModifiers().testFlag(Qt::KeypadModifier);

	unsigned action_modality = 0;
	if (_mod_shift_down)
    action_modality |= Noggit::ActionModalityControllers::eSHIFT;
	if (_mod_ctrl_down)
    action_modality |= Noggit::ActionModalityControllers::eCTRL;
  if (_mod_alt_down)
    action_modality |= Noggit::ActionModalityControllers::eALT;
  if (_mod_num_down)
    action_modality |= Noggit::ActionModalityControllers::eNUM;
  if (_mod_space_down)
    action_modality |= Noggit::ActionModalityControllers::eSPACE;
  if (leftMouse)
    action_modality |= Noggit::ActionModalityControllers::eLMB;
  if (rightMouse)
    action_modality |= Noggit::ActionModalityControllers::eRMB;

  action_modality |= activeTool()->actionModality();
  // if (keyx != 0 || keyy != 0 || keyz != 0)
  //   action_modality |= Noggit::ActionModalityControllers::eTRANSLATE;

  NOGGIT_ACTION_MGR->endActionOnModalityMismatch(action_modality);

  // start unloading tiles
  _world->mapIndex.enterTile (TileIndex (_camera.position));
  if (_unload_tiles)
    _world->mapIndex.unloadTiles (TileIndex (_camera.position));

  dt = std::min(dt, 1.0f);

  math::degrees yaw (-_camera.yaw()._);

  glm::vec3 dir(1.0f, 0.0f, 0.0f);
  glm::vec3 dirUp(1.0f, 0.0f, 0.0f);
  glm::vec3 dirRight(0.0f, 0.0f, 1.0f);
  math::rotate(0.0f, 0.0f, &dir.x, &dir.y, _camera.pitch());
  math::rotate(0.0f, 0.0f, &dir.x, &dir.z, yaw);

  if (_mod_ctrl_down)
  {
    dirUp.x = 0.0f;
    dirUp.y = 1.0f;
    math::rotate(0.0f, 0.0f, &dirUp.x, &dirUp.y, _camera.pitch());
    math::rotate(0.0f, 0.0f, &dirRight.x, &dirRight.y, _camera.pitch());
    math::rotate(0.0f, 0.0f, &dirUp.x, &dirUp.z, yaw);
    math::rotate(0.0f, 0.0f, &dirRight.x, &dirRight.z,yaw);
  }
  else if(!_mod_shift_down)
  {
    math::rotate(0.0f, 0.0f, &dirUp.x, &dirUp.z, yaw);
    math::rotate(0.0f, 0.0f, &dirRight.x, &dirRight.z, yaw);
  }

  // note : selection update most commonly happens in mouseReleaseEvent, which sets leftMouse to false
  bool selection_changed = false;

  // update camera
  if (_display_mode == display_mode::in_3D)
  {
    if (turn)
    {
      _camera.add_to_yaw(math::degrees(turn));
      _camera_moved_since_last_draw = true;
    }
    if (lookat)
    {
      _camera.add_to_pitch(math::degrees(lookat));
      _camera_moved_since_last_draw = true;
    }

    if (moving)
    {
      _camera.move_forward(moving, dt);
      _camera_moved_since_last_draw = true;
    }
    if (strafing)
    {
      _camera.move_horizontal(strafing, dt);
      _camera_moved_since_last_draw = true;
    }
    if (updown)
    {
      _camera.move_vertical(updown, dt);
      _camera_moved_since_last_draw = true;
    }

    if (_camera_moved_since_last_draw)
    {
      if (_fps_mode.get())
      {
        // there is a also hack to update camera when entering mode in void ViewToolbar::add_tool_icon()
        float h = _world->get_ground_height(_camera.position).y;
        _camera.position.y = h + 3.f;
      }
      else if (_camera_collision.get())
      {
        float h = _world.get()->get_ground_height(_camera.position).y;
        if (_camera.position.y < h + 3.f)
        {
          _camera.position.y = h + 3.f;
        }
      }
    }
  }
  else if (_display_mode == display_mode::in_2D)
  {
    //! \todo this is total bullshit. there should be a seperate view and camera class for tilemode
    if (moving)
    {
      _camera.position.z -= dt * _camera.move_speed * moving;
      _camera_moved_since_last_draw = true;
    }
    if (strafing)
    {
      _camera.position.x += dt * _camera.move_speed * strafing;
      _camera_moved_since_last_draw = true;
    }
    if (updown)
    {
      _2d_zoom *= pow(2.0f, dt * updown * 4.0f);
      _2d_zoom = std::max(0.01f, _2d_zoom);
      _camera_moved_since_last_draw = true;
    }
  }

  // udpate MVP after moving camera
  _model_view = model_view(_debug_cam_mode.get());
  _projection = projection();

  // update cursor pos after camera
  auto cur_action = NOGGIT_CUR_ACTION;

  if ((cur_action && !cur_action->getBlockCursor()) || !cur_action)
  {
    if (_locked_cursor_mode.get())
    {
      switch (terrainMode)
      {
      case editing_mode::areaid:
      case editing_mode::impass:
      case editing_mode::holes:
      case editing_mode::object:
        update_cursor_pos();
        break;
      default:
        break;
      }
    }
    else
    {
      update_cursor_pos();
    }
  }

  // _minimap->update(); // causes massive performance issues, should only be done when moving
  Noggit::TickParameters tickParams
  {
      .displayMode = _display_mode,
      .underMap = _world->isUnderMap(_cursor_pos),
      .camera_moved_since_last_draw = _camera_moved_since_last_draw,
      .left_mouse = leftMouse,
      .right_mouse = rightMouse,
      .mod_shift_down = _mod_shift_down,
      .mod_ctrl_down = _mod_ctrl_down,
      .mod_alt_down = _mod_alt_down,
      .mod_num_down = _mod_num_down,
      .dir = dir,
      .dirUp = dirUp,
      .dirRight = dirRight,
  };

  activeTool()->onTick(dt, tickParams);

  auto currentSelection = _world->current_selection();
  if (_world->has_selection())
  {
    // update rotation editor if the selection has changed
    if (lastSelected != currentSelection)
    {
      selection_changed = true;
      emit rotationChanged();
    }
  }

  _world->time += this->mTimespeed * dt;
  _world->animtime += dt * 1000.0f;

  if (_draw_model_animations.get())
  {
    _world->update_models_emitters(dt);
  }

  if (_world->has_selection())
  {
    lastSelected = currentSelection;
  }

  QString status;
  status += ( QString ("tile: %1 %2")
            . arg (std::floor (_camera.position.x / TILESIZE))
            . arg (std::floor (_camera.position.z / TILESIZE))
            );
  status += ( QString ("; coordinates client: (%1, %2, %3), server: (%4, %5, %6)")
            . arg (_camera.position.x, 0, 'f', 2)
            . arg (_camera.position.z, 0, 'f', 2)
            . arg (_camera.position.y, 0, 'f', 2)
            . arg (ZEROPOINT - _camera.position.z, 0, 'f', 2)
            . arg (ZEROPOINT - _camera.position.x, 0, 'f', 2)
            . arg (_camera.position.y, 0, 'f', 2)
            );

  _status_position->setText (status);

  if (currentSelection.size() > 0) // currently disabled, change to == to enable status bar selection
  {
    _status_selection->setText ("");
  }
  else if (currentSelection.size() == 1)
  {
    switch (currentSelection.begin()->index())
    {
    case eEntry_Object:
      {
        auto obj = std::get<selected_object_type>(*currentSelection.begin());

        if (obj->which() == eMODEL)
        {
          auto instance(static_cast<ModelInstance*>(obj));
          _status_selection->setText
              ( QString ("%1: %2")
                    . arg (instance->uid)
                    . arg (QString::fromStdString (instance->model->file_key().stringRepr()))
              );
        }
        else if (obj->which() == eWMO)
        {
          auto instance(static_cast<WMOInstance*>(obj));
          _status_selection->setText
              ( QString ("%1: %2")
                    . arg (instance->uid)
                    . arg (QString::fromStdString (instance->wmo->file_key().stringRepr()))
              );
        }

        break;
      }
    case eEntry_MapChunk:
      {
      auto chunk(std::get<selected_chunk_type>(*currentSelection.begin()).chunk);
        _status_selection->setText
          (QString ("%1, %2").arg (chunk->px).arg (chunk->py));
        break;
      }
    }
  }
  else
  {
	  _status_selection->setText(QString::number(currentSelection.size()) + " objects selected");
  }

  // The detail panel, immediately on a selection change and at most ten times a second while a
  // brush is held.
  //
  // NOGGIT_CUR_ACTION is set for the whole duration of a stroke, so the old condition rebuilt the
  // panel every frame while painting -- and rebuilding it means selected_chunk_type::updateDetails
  // assembling a multi-kilobyte HTML string through a std::stringstream, two linear scans of
  // AreaTable.dbc for the area name, and then a full QTextDocument parse and layout inside
  // QLabel::setText, because DetailInfos.cpp:26 sets Qt::RichText. All of that during exactly the
  // interaction that has to stay responsive, and only for users who keep the panel open --
  // updateDetailInfos itself does nothing when it is hidden (MapView.cpp:2005).
  //
  // The final value still lands: _detail_infos_stale remembers that a frame was skipped, and the
  // first frame after the stroke ends runs the update it owes. Ten hertz is a readable refresh for
  // a panel of numbers; it is not a rate a reader can tell from sixty by looking.
  constexpr qint64 DETAIL_INFOS_MIN_INTERVAL_MS = 100;

  if (selection_changed)
  {
    updateDetailInfos(); // checks if sel changed
    _detail_infos_clock.restart();
    _detail_infos_stale = false;
  }
  else if (NOGGIT_CUR_ACTION)
  {
    if ( !_detail_infos_clock.isValid()
      || _detail_infos_clock.elapsed() >= DETAIL_INFOS_MIN_INTERVAL_MS
       )
    {
      updateDetailInfos();
      _detail_infos_clock.restart();
      _detail_infos_stale = false;
    }
    else
    {
      _detail_infos_stale = true;
    }
  }
  else if (_detail_infos_stale)
  {
    updateDetailInfos();
    _detail_infos_stale = false;
  }

  if (selection_changed)
  {
      emit selectionUpdated(currentSelection);
      // updateDetailInfos();
  }

  // Only re-derived when the camera crosses into a different area. See _status_area_name in the
  // header for what the old unconditional call cost per frame.
  {
    unsigned int const area_id (_world->getAreaID (_camera.position));

    if (!_status_area_known || area_id != _status_area_id)
    {
      _status_area_id = area_id;
      _status_area_known = true;
      _status_area_name
        = QString::fromStdString (gAreaDB.getAreaFullName (static_cast<int>(area_id)));
    }

    _status_area->setText (_status_area_name);
  }

  {
    int time ((static_cast<int>(_world->time) % 2880) / 2);
    std::stringstream timestrs;
    timestrs << "Time: " << (time / 60) << ":" << std::setfill ('0')
             << std::setw (2) << (time % 60);


    timestrs << ", Pres: " << _tablet_manager->pressure();

    _status_time->setText (QString::fromStdString (timestrs.str()));
  }

  _last_fps_update += dt;

  // update fps every sec
  if (_last_fps_update > 1.f && !_last_frame_durations.empty())
  {
    auto avg_frame_duration
      ( std::accumulate ( _last_frame_durations.begin()
                        , _last_frame_durations.end()
                        , 0.
                        )
      / qreal (_last_frame_durations.size())
      );
    _status_fps->setText ( "FPS: " + QString::number (int (1. / avg_frame_duration)) 
                         + " - Average frame time: " + QString::number(avg_frame_duration*1000.0) + "ms"
                         );

    _last_frame_durations.clear();
    _last_fps_update = 0.f;
  }

  _status_culling->setText ( "Loaded tiles: " + QString::number(_world->getNumLoadedTiles())
                         + ", Rendered tiles: " + QString::number(_world->getNumRenderedTiles())
                         + "\t Loaded objects: " + QString::number(_world->getModelInstanceStorage().getTotalModelsCount())
                         + ", Rendered objects: " + QString::number(_world->getNumRenderedObjects())
  );
}

glm::vec4 MapView::normalized_device_coords (int x, int y) const
{
  return {2.0f * x / width() - 1.0f, 1.0f - 2.0f * y / height(), 0.0f, 1.0f};
}

float MapView::aspect_ratio() const
{
  return float (width()) / float (height());
}

math::ray MapView::intersect_ray() const
{
  float mx = _last_mouse_pos.x(), mz = _last_mouse_pos.y();

  if (_display_mode == display_mode::in_3D)
  {
    // during rendering we multiply perspective * view
    // so we need the same order here and then invert.
    glm::mat4x4 const invertedViewMatrix = glm::inverse(_projection * _model_view);
    auto normalisedView = invertedViewMatrix * normalized_device_coords(mx, mz);

    auto pos = glm::vec3(normalisedView.x / normalisedView.w, normalisedView.y / normalisedView.w, normalisedView.z / normalisedView.w);

    return { _camera.position, pos - _camera.position };
  }
  else
  {
    glm::vec3 const pos
    ( _camera.position.x - (width() * 0.5f - mx) * _2d_zoom
    , _camera.position.y
    , _camera.position.z - (height() * 0.5f - mz) * _2d_zoom
    );
    
    return { pos, glm::vec3(0.f, -1.f, 0.f) };
  }
}

selection_result MapView::intersect_result(bool terrain_only)
{
  selection_result results
  ( _world->intersect 
    ( glm::transpose(_model_view)
    , intersect_ray()
    , terrain_only
    , terrainMode == editing_mode::object || terrainMode == editing_mode::minimap
    , _draw_terrain.get()
    , _draw_wmo.get()
    , _draw_models.get()
    , _draw_hidden_models.get()
    , _draw_wmo_exterior.get()
    , _draw_model_animations.get()
    )
  );

  std::sort ( results.begin()
            , results.end()
            , [](selection_entry const& lhs, selection_entry const& rhs)
              {
                return lhs.first < rhs.first;
              }
            );

  return std::move(results);
}

void MapView::doSelection (bool selectTerrainOnly, bool mouseMove)
{
  // Clicking a gizmo handle must not also be a selection click. gizmoIsDrawn() covers both the
  // object gizmo and the spawn gizmo -- get_selected_model_count() is 0 when only a database spawn
  // is selected -- and, being false when no gizmo is on screen, is also what stops a stale
  // ImGuizmo::IsOver() from blocking selection in the region a dismissed gizmo used to occupy.
  if (gizmoIsDrawn() && (_transform_gizmo.isUsing() || _transform_gizmo.isOver()))
    return;

  selection_result results(intersect_result(selectTerrainOnly));

  if (results.empty())
  {
    _world->reset_selection();
  }
  else
  {
    auto const& hit (results.front().second);

    if (terrainMode == editing_mode::object || terrainMode == editing_mode::minimap)
    {
      float radius = activeTool()->brushRadius();

      if (_mod_shift_down)
      {
        if (hit.index() == eEntry_Object)
        {
          if (!_world->is_selected(hit))
          {
            _world->add_to_selection(hit);
          }
          else if (!mouseMove)
          {
            _world->remove_from_selection(hit);
          }
        }
        else if (hit.index() == eEntry_MapChunk)
        {
          _world->range_add_to_selection(_cursor_pos, radius, false);
        }
      }
      else if (_mod_ctrl_down)
      {
        if (hit.index() == eEntry_MapChunk)
        {
          _world->range_add_to_selection(_cursor_pos, radius, true);
        }
      }
      else if (!_mod_space_down && !_mod_alt_down && !_mod_ctrl_down)
      {
        // objectEditor->update_selection(_world.get());
        _world->reset_selection();
        _world->add_to_selection(hit);
      }
    }
    else if (hit.index() == eEntry_MapChunk && !mouseMove)
    {
      _world->reset_selection();
      _world->add_to_selection(hit);
    }

    auto action = NOGGIT_CUR_ACTION;

    if (!action || (!action->getBlockCursor()) || !_locked_cursor_mode.get())
    {
      _cursor_pos = hit.index() == eEntry_Object ? std::get<selected_object_type>(hit)->pos
                                                 : hit.index() == eEntry_MapChunk ? std::get<selected_chunk_type>(hit).position
                                                                                  : throw std::logic_error("bad variant");
    }

  }

  emit rotationChanged();
}

void MapView::update_cursor_pos()
{
  static bool buffer_switch = false;

  if (false && terrainMode != editing_mode::holes) // figure out why this does not work on every hardware.
  {
    float mx = _last_mouse_pos.x(), mz = _last_mouse_pos.y();

    //gl.readBuffer(GL_FRONT);
    gl.bindBuffer(GL_PIXEL_PACK_BUFFER, _buffers[static_cast<unsigned>(buffer_switch)]);

    gl.readPixels(mx, height() - mz - 1, 1, 1, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, 0);

    gl.bindBuffer(GL_PIXEL_PACK_BUFFER, _buffers[static_cast<unsigned>(!buffer_switch)]);
    GLushort* ptr = static_cast<GLushort*>(gl.mapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY));

    buffer_switch = !buffer_switch;

    if(ptr)
    {
      glm::vec4 viewport = glm::vec4(0, 0, width(), height());
      glm::vec3 wincoord = glm::vec3(mx, height() - mz - 1, static_cast<float>(*ptr) / std::numeric_limits<unsigned short>::max());

      // glm::mat4x4 model_view_ = model_view();
      // glm::mat4x4 projection_ = projection();

      glm::vec3 objcoord = glm::unProject(wincoord, _model_view, _projection, viewport);


      TileIndex tile({objcoord.x, objcoord.y, objcoord.z});

      if (!_world->mapIndex.tileLoaded(tile))
      {
        gl.unmapBuffer(GL_PIXEL_PACK_BUFFER);
        gl.bindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        return;
      }

      _cursor_pos = {objcoord.x, objcoord.y, objcoord.z};

      gl.unmapBuffer(GL_PIXEL_PACK_BUFFER);
    }

    gl.bindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    return;
  }

  // use raycasting for holes

  selection_result results (intersect_result (true));

  if (!results.empty())
  {
    auto const& hit(results.front().second);
    // hit cannot be something else than a chunk
    auto const& chunkHit = std::get<selected_chunk_type>(hit);
    _cursor_pos = chunkHit.position;

  }
}

glm::mat4x4 MapView::model_view(bool use_debug_cam) const
{
  if (_display_mode == display_mode::in_2D)
  {
    glm::vec3 eye = use_debug_cam ? _debug_cam.position : _camera.position;
    glm::vec3 target = eye;
    target.y -= 1.f;
    target.z -= 0.001f;
    auto center = target;
    auto up = glm::vec3(0.f, 1.f, 0.f);

    return glm::lookAt(eye, target, up);
  }
  else
  {
    if (use_debug_cam)
    {
        return _debug_cam.look_at_matrix();
    }
    else
    {
        return _camera.look_at_matrix();
    }
  }
}

glm::mat4x4 MapView::projection() const
{
  // float far_z = _settings->value("view_distance", 2000.f).toFloat() + 1.f; // don't access qsettings in mainloop, it's slow
  float far_z = _world->renderer()->_view_distance - TILE_RADIUS + 1.0f;

  if (_display_mode == display_mode::in_2D)
  {
    float half_width = width() * 0.5f * _2d_zoom;
    float half_height = height() * 0.5f * _2d_zoom;

    return glm::ortho(-half_width, half_width, -half_height, half_height, -1.f, far_z);
  }
  else
  {
    return glm::perspective(_camera.fov()._, aspect_ratio(), _fps_mode.get() ? 0.1f : 1.f, far_z);
  }
}

void MapView::draw_map()
{
  ZoneScoped;
  //! \ todo: make the current tool return the radius
  float radius = 0.0f, inner_radius = 0.0f, angle = 0.0f, orientation = 0.0f;
  glm::vec3 ref_pos;
  bool angled_mode = false, use_ref_pos = false;

  _cursorType = CursorType::CIRCLE;

  eTerrainType terrainType = eTerrainType_Flat;
  bool show_unpaintable_chunks = false;
  int displayed_water_layer = -1;
  auto cursorColor = cursor_color;
  MinimapRenderSettings minimapRenderSettings;

  auto draw_parameters = activeTool()->drawParameters();
  radius = draw_parameters.radius;
  inner_radius = draw_parameters.inner_radius;
  _cursorType = draw_parameters.cursor_type;
  terrainType = draw_parameters.terrain_type;
  angle = draw_parameters.angle;
  orientation = draw_parameters.orientation;
  ref_pos = draw_parameters.ref_pos;
  angled_mode = draw_parameters.angled_mode;
  use_ref_pos = draw_parameters.use_ref_pos;
  show_unpaintable_chunks = draw_parameters.show_unpaintable_chunks;
  displayed_water_layer = draw_parameters.displayed_water_layer;
  cursorColor = draw_parameters.cursor_color;
  minimapRenderSettings = draw_parameters.minimapRenderSettings;

  bool debug_cam = _debug_cam_mode.get();

  // math::frustum frustum(model_view(debug_cam) * projection());
  _model_view = model_view(debug_cam);
  _projection = projection();

  //! \note Select terrain below mouse, if no item selected or the item is map.
  if (!(_world->has_selection()
    || _locked_cursor_mode.get()))
  {
    doSelection(true);
  }

  if (_camera_moved_since_last_draw)
  {
      _minimap->update();
  }

  bool show_unpaintable = _classic_ui ? show_unpaintable_chunks : _left_sec_toolbar->showUnpaintableChunk();



  WorldRenderParams renderParams;

  renderParams.cursorRotation = _cursorRotation;
  renderParams.cursor_type = _cursorType;
  renderParams.brush_radius = radius;
  renderParams.show_unpaintable_chunks = show_unpaintable;
  renderParams.draw_only_inside_light_sphere = _left_sec_toolbar->drawOnlyInsideSphereLight();
  renderParams.draw_wireframe_light_sphere = _left_sec_toolbar->drawWireframeSphereLight();
  renderParams.alpha_light_sphere = _left_sec_toolbar->getAlphaSphereLight();
  renderParams.inner_radius_ratio = inner_radius;
  renderParams.angle = angle;
  renderParams.orientation = orientation;
  renderParams.use_ref_pos = use_ref_pos;
  renderParams.angled_mode = angled_mode;
  renderParams.draw_paintability_overlay = terrainMode == editing_mode::paint;
  renderParams.editing_mode = terrainMode;
  renderParams.camera_moved = debug_cam ? false : _camera_moved_since_last_draw;
  renderParams.draw_mfbo = _draw_mfbo.get();
  renderParams.draw_terrain = _draw_terrain.get();
  renderParams.draw_wmo = _draw_wmo.get();
  renderParams.draw_water = _draw_water.get();
  renderParams.draw_wmo_doodads = _draw_wmo_doodads.get();
  renderParams.draw_models = _draw_models.get();
  renderParams.draw_model_animations = _draw_model_animations.get();
  renderParams.draw_models_with_box = _draw_models_with_box.get();
  renderParams.draw_hidden_models = _draw_hidden_models.get();
  renderParams.draw_sky = _draw_sky.get();
  renderParams.draw_skybox = _draw_skybox.get();
  renderParams.draw_fog = _draw_fog.get();
  renderParams.ground_editing_brush = terrainType;
  renderParams.water_layer = displayed_water_layer;
  renderParams.display_mode = _display_mode;
  renderParams.draw_occlusion_boxes = _draw_occlusion_boxes.get();
  // Null until the load action has run, which is what the renderer checks -- so a session that
  // never loads spawns, or a build with no database, needs no special case in the render path.
  renderParams.draw_db_spawns = _draw_db_spawns.get();
  renderParams.db_spawns = _db_spawn_scene.get();
  renderParams.minimap_render = false;
  renderParams.draw_wmo_exterior = _draw_wmo_exterior.get();
  renderParams.render_select_m2_aabb = _render_m2_aabb;
  renderParams.render_select_m2_collission_bbox = _render_m2_collission_bbox;
  renderParams.render_select_wmo_aabb = _render_wmo_aabb;
  renderParams.render_select_wmo_groups_bounds = _render_wmo_groups_bounds;

  _world->renderer()->draw (
                  _model_view
                , _projection
                , _cursor_pos
                , cursorColor
                , ref_pos
                , _camera.position
                , &minimapRenderSettings
                , renderParams
                );

  // reset after each world::draw call
  _camera_moved_since_last_draw = false;
}

void MapView::keyPressEvent (QKeyEvent *event)
{
  if (event->key() == Qt::Key_Space)
  {
    _mod_space_down = true;
  }

  size_t const modifier
    ( ((event->modifiers() & Qt::ShiftModifier) ? MOD_shift : 0)
    | ((event->modifiers() & Qt::ControlModifier) ? MOD_ctrl : 0)
    | ((event->modifiers() & Qt::AltModifier) ? MOD_alt : 0)
    | ((event->modifiers() & Qt::MetaModifier) ? MOD_meta : 0)
    | ((event->modifiers() & Qt::KeypadModifier) ? MOD_num : 0)
    | (_mod_space_down ? MOD_space : 0)
    );

  for (auto&& hotkey : hotkeys)
  {
    if (event->key() == hotkey.key && modifier == hotkey.modifiers && hotkey.condition())
    {
      makeCurrent();
      OpenGL::context::scoped_setter const _ (::gl, context());

      hotkey.onPress();
      return;
    }
  }

  checkInputsSettings();

  // movement
  if (event->key() == _inputs[0])
  {
    moving = 1.0f;
  }
  if (event->key() == _inputs[1])
  {
    moving = -1.0f;
  }

  if (event->key() == Qt::Key_Up)
  {
    lookat = 0.75f;
  }
  if (event->key() == Qt::Key_Down)
  {
    lookat = -0.75f;
  }

  if (event->key() == Qt::Key_Right)
  {
    turn = 0.75f;
  }
  if (event->key() == Qt::Key_Left)
  {
    turn = -0.75f;
  }

  if (event->key() == _inputs[2])
  {
    strafing = 1.0f;
  }
  if (event->key() == _inputs[3])
  {
    strafing = -1.0f;
  }

  if (event->key() == _inputs[4])
  {
    updown = 1.0f;
  }
  if (event->key() == _inputs[5])
  {
    updown = -1.0f;
  }

  if (event->key() == Qt::Key_Home)
  {
	  _camera.position = glm::vec3(_cursor_pos.x, _cursor_pos.y + 50, _cursor_pos.z);
    _camera_moved_since_last_draw = true;
  }

  if (event->key() == Qt::Key_L)
  {
    freelook = true;
  }

  if (_display_mode == display_mode::in_2D)
  {
    TileIndex cur_tile = TileIndex(_camera.position);

    if (event->key() == Qt::Key_Up)
    {
      auto next_z = cur_tile.z - 1;
      _camera.position = glm::vec3((cur_tile.x * TILESIZE) + (TILESIZE / 2), _camera.position.y, (next_z * TILESIZE) + (TILESIZE / 2));
      _camera_moved_since_last_draw = true;
    }
    else if (event->key() == Qt::Key_Down)
    {
      auto next_z = cur_tile.z + 1;
      _camera.position = glm::vec3((cur_tile.x * TILESIZE) + (TILESIZE / 2), _camera.position.y, (next_z * TILESIZE) + (TILESIZE / 2));
      _camera_moved_since_last_draw = true;
    }
    else if (event->key() == Qt::Key_Left)
    {
      auto next_x = cur_tile.x - 1;
      _camera.position = glm::vec3((next_x * TILESIZE) + (TILESIZE / 2), _camera.position.y, (cur_tile.z * TILESIZE) + (TILESIZE / 2));
      _camera_moved_since_last_draw = true;
    }
    else if (event->key() == Qt::Key_Right)
    {
      auto next_x = cur_tile.x + 1;
      _camera.position = glm::vec3((next_x * TILESIZE) + (TILESIZE / 2), _camera.position.y, (cur_tile.z * TILESIZE) + (TILESIZE / 2));
      _camera_moved_since_last_draw = true;
    }

  }

  if (_gizmo_on.get() && !_transform_gizmo.isUsing())
  {
    if (!_change_operation_mode && event->key() == Qt::Key_Space)
    {
      if (_gizmo_operation == ImGuizmo::OPERATION::TRANSLATE)
      {
        updateGizmoOverlay(ImGuizmo::OPERATION::ROTATE);
      }
      else if (_gizmo_operation == ImGuizmo::OPERATION::ROTATE)
      {
        updateGizmoOverlay(ImGuizmo::OPERATION::SCALE);
      }
      else
      {
        updateGizmoOverlay(ImGuizmo::OPERATION::TRANSLATE);
      }

      _change_operation_mode = true;
    }
  }
}

void MapView::keyReleaseEvent (QKeyEvent* event)
{
  if (event->key() == Qt::Key_Space)
    _mod_space_down = false;

  if (_change_operation_mode && event->key() == Qt::Key_Space)
    _change_operation_mode = false;

  checkInputsSettings();

  size_t const modifier
  (((event->modifiers() & Qt::ShiftModifier) ? MOD_shift : 0)
      | ((event->modifiers() & Qt::ControlModifier) ? MOD_ctrl : 0)
      | ((event->modifiers() & Qt::AltModifier) ? MOD_alt : 0)
      | ((event->modifiers() & Qt::MetaModifier) ? MOD_meta : 0)
      | ((event->modifiers() & Qt::KeypadModifier) ? MOD_num : 0)
      | (_mod_space_down ? MOD_space : 0)
  );
  for (auto&& hotkey : hotkeys)
  {
      auto k = event->key();
      if (k == hotkey.key && modifier == hotkey.modifiers && hotkey.condition())
      {
          makeCurrent();
          OpenGL::context::scoped_setter const _(::gl, context());

          hotkey.onRelease();
          return;
      }
  }

  // movement
  if (event->key() == _inputs[0] || event->key() == _inputs[1])
  {
    moving = 0.0f;
  }

  if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)
  {
    lookat = 0.0f;
  }

  if (event->key() == Qt::Key_Right || event->key() == Qt::Key_Left)
  {
    turn  = 0.0f;
  }

  if (event->key() == _inputs[2] || event->key() == _inputs[3])
  {
    strafing  = 0.0f;
  }

  if (event->key() == _inputs[4] || event->key() == _inputs[5])
  {
    updown  = 0.0f;
  }

  if (event->key() == Qt::Key_L || event->key() == Qt::Key_Minus)
  {
    freelook = false;
  }

}

void MapView::checkInputsSettings()
{
  QString _locale = _settings->value("keyboard_locale", "QWERTY").toString();

  // default is QWERTY
  _inputs = std::array<Qt::Key, 6>{Qt::Key_W, Qt::Key_S, Qt::Key_D, Qt::Key_A, Qt::Key_Q, Qt::Key_E};

  if (_locale == "AZERTY")
  {
      _inputs = std::array<Qt::Key, 6>{Qt::Key_Z, Qt::Key_S, Qt::Key_D, Qt::Key_Q, Qt::Key_A, Qt::Key_E};
  }
}

void MapView::focusOutEvent (QFocusEvent*)
{
  _mod_alt_down = false;
  _mod_ctrl_down = false;
  _mod_shift_down = false;
  _mod_space_down = false;
  _mod_num_down = false;

  moving = 0.0f;
  lookat = 0.0f;
  turn = 0.0f;
  strafing = 0.0f;
  updown = 0.0f;

  leftMouse = false;
  rightMouse = false;
  look = false;
  freelook = false;

  activeTool()->onFocusLost();
}

void MapView::mouseMoveEvent (QMouseEvent* event)
{
  //! \todo:  move the function call requiring a context in tick ?
  makeCurrent();
  OpenGL::context::scoped_setter const _ (::gl, context());
  QLineF const relative_movement (_last_mouse_pos, event->pos());

  if ((look || freelook) && !(_mod_shift_down || _mod_ctrl_down || _mod_alt_down || _mod_space_down))
  {
    _camera.add_to_yaw(math::degrees(relative_movement.dx() / XSENS));
    _camera.add_to_pitch(math::degrees(mousedir * relative_movement.dy() / YSENS));
    _camera_moved_since_last_draw = true;
  }

  Noggit::MouseMoveParameters params{
    .displayMode = _display_mode,
    .left_mouse = leftMouse,
    .right_mouse = rightMouse,
    .mod_shift_down = _mod_shift_down,
    .mod_ctrl_down = _mod_ctrl_down,
    .mod_alt_down = _mod_alt_down,
    .mod_num_down = _mod_num_down,
    .mod_space_down = _mod_space_down,
    .relative_movement = relative_movement,
    .mouse_position = event->pos()
  };

  activeTool()->onMouseMove(params);

  if (_display_mode == display_mode::in_2D && leftMouse && _mod_alt_down && _mod_shift_down)
  {
    strafing = ((relative_movement.dx() / XSENS) / -1) * 5.0f;
    moving = (relative_movement.dy() / YSENS) * 5.0f;
  }

  if (_display_mode == display_mode::in_2D && rightMouse && _mod_shift_down)
  {
    updown = (relative_movement.dy() / YSENS);
  }

  _last_mouse_pos = event->pos();
}

void MapView::change_selected_wmo_nameset(int set)
{
    auto last_entry = _world->get_last_selected_model();
    if (last_entry)
    {
        if (last_entry.value().index() != eEntry_Object)
        {
            return;
        }
        auto obj = std::get<selected_object_type>(last_entry.value());
        if (obj->which() == eWMO)
        {
            WMOInstance* wmo = static_cast<WMOInstance*>(obj);
            wmo->change_nameset(set);
            _world->updateTilesWMO(wmo, model_update::none); // needed?
            auto tiles = wmo->getTiles();
            for (auto tile : tiles)
            {
                tile->changed = true;
            }
        }
    }
}

void MapView::change_selected_wmo_doodadset(int set)
{
  for (auto& selection : _world->current_selection())
  {
    if (selection.index() != eEntry_Object)
      continue;

    auto obj = std::get<selected_object_type>(selection);

    if (obj->which() == eWMO)
    {
      auto wmo = static_cast<WMOInstance*>(obj);
      wmo->change_doodadset(set);
      _world->updateTilesWMO(wmo, model_update::none);
      auto tiles = wmo->getTiles();
      for (auto tile : tiles)
      {
        tile->changed = true;
      }
    }
  }
}

void MapView::mousePressEvent(QMouseEvent* event)
{
  if(event->source() == Qt::MouseEventNotSynthesized)
  {
    _tablet_manager->setIsActive(false);
  }

  makeCurrent();
  OpenGL::context::scoped_setter const _(::gl, context());

  // Clicking a database spawn selects it, ahead of the active tool.
  //
  // Only when the overlay is on and the spawn panel is open, so this cannot steal clicks from
  // anyone not using it. Shift is the modifier because a plain click still has to reach the
  // terrain tools -- selecting a creature must not stop you painting the ground under it.
  // The gizmo gets the click first when the cursor is on a handle. Without this a shift-click that
  // starts a drag also re-picks whatever is behind the handle, and the drag ends up moving a
  // different spawn than the one that was grabbed. Same guard as doSelection, including the term
  // this originally lacked: gizmoIsDrawn().
  //
  // Without it the guard was true whenever the cursor happened to fall in the screen rectangle of
  // the last gizmo that was manipulated, because IsOver() answers from geometry only Manipulate
  // refreshes and nothing invalidates when the gizmo stops being drawn. That silently swallowed
  // both the spawn pick below and the move-mode placement after it -- the click reached neither
  // branch nor a visible gizmo, and fell through to the active tool.
  bool const gizmo_has_the_click
    (gizmoIsDrawn() && (_transform_gizmo.isUsing() || _transform_gizmo.isOver()));

  // Clicking a light sphere selects it, ahead of the active tool, in light mode only.
  //
  // A plain left click and not the spawn overlay's shift-click: LightTool overrides no mouse hook
  // at all, so in this mode the left button is otherwise unused, and demanding a modifier to
  // select the only thing the mode edits would be gratuitous. Shift is deliberately excluded here
  // so the database spawn pick below keeps it, and ctrl and alt are excluded because they are
  // already the camera and drag-select modifiers -- three modes cannot share one button unless
  // each says which modifier state it wants.
  if (event->button() == Qt::LeftButton && terrainMode == editing_mode::light
   && !_mod_shift_down && !_mod_ctrl_down && !_mod_alt_down && !gizmo_has_the_click)
  {
    int const picked (pickLight());

    if (picked)
    {
      selectLight(picked);
      return;
    }
  }

  if (event->button() == Qt::LeftButton && _mod_shift_down && _db_spawn_scene
   && _db_spawn_panel && _draw_db_spawns.get() && !gizmo_has_the_click)
  {
    Noggit::Database::SpawnRef const picked (pickDatabaseSpawn());

    if (picked.valid())
    {
      _db_spawn_scene->setSelected(picked);
      _db_spawn_panel->selectSpawn(picked);
      _needs_redraw = true;
      return;
    }
  }

  // Placing a database spawn takes the click before the active tool sees it, and returns.
  //
  // Ahead of the tool rather than after it because otherwise the terrain tool underneath would
  // also act on the same click -- raising ground where the user meant to put a creature. Move
  // mode is off by default and is a deliberate toggle, so this cannot surprise anyone who is not
  // using it.
  if (event->button() == Qt::LeftButton && _db_spawn_panel && _db_spawn_panel->moveMode()
   && !gizmo_has_the_click)
  {
    Noggit::Database::SpawnRef const spawn (_db_spawn_panel->selectedSpawn());

    if (spawn.valid() && _db_spawn_scene)
    {
      if (_db_spawn_scene->moveTo(spawn, _cursor_pos))
      {
        _db_spawn_panel->refresh();
        _needs_redraw = true;
        return;
      }
    }
  }

  // Placing a NEW spawn, on the same terms as move mode: ahead of the active tool, off by
  // default, and behind a deliberate toggle. The two modes are mutually exclusive in the panel,
  // so at most one of these branches can be armed at a time.
  //
  // _cursor_pos is the terrain cursor, i.e. where the click actually landed on the ground --
  // the same value move mode uses. Taking the camera position instead would put the spawn inside
  // the viewer.
  if (event->button() == Qt::LeftButton && _db_spawn_panel && _db_spawn_panel->placeMode()
   && !gizmo_has_the_click)
  {
    // Interactive: a human clicked, so a failure -- an entry that does not exist, a template
    // with no model -- has to say so rather than doing nothing visible.
    createDatabaseSpawn( _db_spawn_panel->placeCreature()
                       , _db_spawn_panel->placeEntry()
                       , _cursor_pos
                       , true
                       );

    _needs_redraw = true;
    return;
  }

  activeTool()->onMousePress({
      .button = event->button(),
      .mouse_position = event->pos(),
      .mod_shift_down = _mod_shift_down,
      .mod_ctrl_down = _mod_ctrl_down,
      .mod_alt_down = _mod_alt_down,
      .mod_num_down = _mod_num_down,
      .mod_space_down = _mod_space_down,
      });

  switch (event->button())
  {
  case Qt::LeftButton:
    leftMouse = true;
    break;

  case Qt::RightButton:
    rightMouse = true;
    break;

  default:
    break;
  }

  if (leftMouse && terrainMode == editing_mode::minimap && !_mod_ctrl_down)
  {
      _drag_start_pos = event->pos();
      _needs_redraw = true;
  }

  if (rightMouse)
  {
    _right_click_pos = event->pos();
    look = true;
  }
}

void MapView::wheelEvent (QWheelEvent* event)
{
  //! \todo: move the function call requiring a context in tick ?
  makeCurrent();
  OpenGL::context::scoped_setter const _ (::gl, context());

  auto&& delta_for_range
    ( [&] (float range)
      {
        //! \note / 8.f for degrees, / 40.f for smoothness
        return (_mod_ctrl_down ? 0.01f : 0.1f) 
          * range 
          // alt = horizontal delta
          * (_mod_alt_down ? event->angleDelta().x() : event->angleDelta().y())
          / 320.f
          ;
      }
    );

  // Ctrl + wheel resizes the selected light, both radii together.
  //
  // Ahead of the active tool and returning, because in light mode there is no competing consumer:
  // LightTool overrides no wheel hook, and the only other reader of the wheel here is the
  // delta_for_range lambda above, which nothing calls. One notch is 120 units of angleDelta, so
  // this is 5% per notch -- a light goes from 200 to 400 yards in about fifteen notches, which is
  // roughly a second of scrolling.
  if (terrainMode == editing_mode::light && _mod_ctrl_down && !_mod_shift_down && !_mod_alt_down)
  {
    float const notches (static_cast<float>(event->angleDelta().y()) / 120.0f);

    if (notches != 0.0f && scaleSelectedLightRadii(std::pow(1.05f, notches)))
    {
      event->accept();
      return;
    }
  }

  Noggit::MouseWheelParameters params
  {
      .event = *event,
      .mod_shift_down = _mod_shift_down,
      .mod_ctrl_down = _mod_ctrl_down,
      .mod_alt_down = _mod_alt_down,
      .mod_num_down = _mod_num_down,
      .mod_space_down = _mod_space_down,
  };
  activeTool()->onMouseWheel(params);
}

void MapView::mouseReleaseEvent (QMouseEvent* event)
{
  makeCurrent();
  OpenGL::context::scoped_setter const _(::gl, context());

  activeTool()->onMouseRelease(
  {
      .button = event->button(),
      .mouse_position = event->pos(),
      .mod_ctrl_down = _mod_ctrl_down,
  });

  // End of a spawn gizmo drag: rebuild the panel once, now that the cursor is no longer on a row.
  //
  // Done here rather than in the frame that applied the move because refresh() clears and
  // repopulates the QListWidget and drives currentRowChanged through it; running that sixty times
  // a second scrolls the list out from under the pointer. Deferring it also means the "* " dirty
  // marker and the facing spin box are updated exactly once per drag.
  if (event->button() == Qt::LeftButton && _spawn_gizmo_dragging)
  {
    _spawn_gizmo_dragging = false;

    if (_db_spawn_panel)
    {
      _db_spawn_panel->refresh();
    }

    _needs_redraw = true;
  }

  // End of a light gizmo drag, for the same reason and with the same timing: writing the new
  // position back into the panel's three spin boxes on every frame of a drag would fight the user
  // for the caret and re-enter their valueChanged handlers sixty times a second.
  if (event->button() == Qt::LeftButton && _light_gizmo_dragging)
  {
    _light_gizmo_dragging = false;

    if (_light_editor)
    {
      _light_editor->refreshSelectedLightFields();
    }

    _needs_redraw = true;
  }

  switch (event->button())
  {
  case Qt::LeftButton:
    leftMouse = false;

    if (_display_mode == display_mode::in_2D)
    {
      strafing = 0;
      moving = 0;
    }

    if (terrainMode == editing_mode::minimap )
    {
        if (!_mod_ctrl_down)
        {
            auto drag_end_pos = event->pos();

            if (_drag_start_pos != drag_end_pos && !ImGuizmo::IsUsing())
            {
                const std::array<glm::vec2, 2> selection_box
                {
                    glm::vec2(std::min(_drag_start_pos.x(), drag_end_pos.x()), std::min(_drag_start_pos.y(), drag_end_pos.y())),
                    glm::vec2(std::max(_drag_start_pos.x(), drag_end_pos.x()), std::max(_drag_start_pos.y(), drag_end_pos.y()))
                };
                // _world->select_objects_in_area(selection_box, !_mod_shift_down, model_view(), projection(), width(), height(), objectEditor->drag_selection_depth(), _camera.position);
                _world->select_objects_in_area(selection_box, !_mod_shift_down, _model_view, _projection, width(), height(), 50000.0f, _camera.position);
            }
            else // Do normal selection when we just clicked
            {
                doSelection(false);
            }
        }
        else
        {
            doSelection(true);
        }
    }

    break;

  case Qt::RightButton:
    rightMouse = false;

    look = false;

    if (_display_mode == display_mode::in_2D)
      updown = 0;

    // // may need to be done in constructor of widget
    // this->setContextMenuPolicy(Qt::CustomContextMenu); 
    // connect(this, SIGNAL(customContextMenuRequested(const QPoint&)),
    //     this, SLOT(ShowContextMenu(const QPoint&)));



    break;

  default:
    break;
  }
}

void MapView::save(save_mode mode)
{
  bool save = true;

  activeTool()->saveSettings();

  if (AsyncLoader::instance->important_object_failed_loading())
  {
    save = false;
    QPushButton *yes, *no, *show_panel;

    auto const& missing_log = Noggit::MissingPlacementLog::instance();

    QMessageBox first_warning;
    first_warning.setIcon(QMessageBox::Critical);
    first_warning.setWindowIcon(QIcon (":/icon"));
    first_warning.setWindowTitle("Some models couldn't be loaded");

    // Was: "Check the log file for the list of model errors and fix them."
    //
    // That sentence was the entire user-facing account of a broken map, and it asked the mapper
    // to close the editor, find log.txt in the working directory and read it -- at the one moment
    // they are least able to act on it, because the dialog is modal and the map is about to be
    // written. It named nothing and counted nothing.
    //
    // It now says what failed and offers the panel that lists it. The count comes from
    // MissingPlacementLog, which is filled at the point of failure
    // (AsyncObject::error_on_loading), so it is the same set of files the panel shows.
    QString first_text
      ( "Error:\nSome models could not be loaded and saving will cause collision and culling"
        " issues, this is most likely caused by missing or corrupted models.\n"
      );

    if (missing_log.fileCount() > 0)
    {
      first_text += QString("\n%1 file%2 could not be loaded")
                      .arg(missing_log.fileCount())
                      .arg(missing_log.fileCount() == 1 ? "" : "s");

      if (missing_log.totalPlacementCount() > 0)
      {
        first_text += QString(", affecting %1 placement%2 found so far")
                        .arg(missing_log.totalPlacementCount())
                        .arg(missing_log.totalPlacementCount() == 1 ? "" : "s");
      }

      first_text += ".\n";
    }

    first_text += "\nWould you still like to save ?";

    first_warning.setText(first_text);

    // roles are swapped to force the user to pay attention and both are "accept" roles so that escape does nothing
    no = first_warning.addButton("No", QMessageBox::ButtonRole::AcceptRole);
    yes = first_warning.addButton("Yes", QMessageBox::ButtonRole::YesRole);
    show_panel = first_warning.addButton("Show missing objects", QMessageBox::ButtonRole::HelpRole);
    first_warning.setDefaultButton(no);

    first_warning.exec();

    if (first_warning.clickedButton() == show_panel)
    {
      // Cancels the save, deliberately. The mapper asked to look at the problem, and quietly
      // writing the ADTs behind that request is exactly what this dialog exists to prevent.
      //
      // Deferred to the next event-loop turn rather than shown here. save() is reached from menu
      // actions today, but showing a dock resizes the main window and can drive a repaint, and
      // the rule in this renderer is that nothing which can re-enter drawing is done inline from
      // a path that might one day be called during one. QTimer::singleShot(0, ...) is how that is
      // spelled everywhere else here.
      QTimer::singleShot(0, this, [this]
        {
          if (_missing_objects_dock)
          {
            _missing_objects_dock->show();
            _missing_objects_dock->raise();
          }

          if (_missing_objects_panel)
          {
            _missing_objects_panel->refresh();
          }
        });

      return;
    }

    if (first_warning.clickedButton() == yes)
    {
      QMessageBox second_warning;
      second_warning.setIcon(QMessageBox::Warning);
      second_warning.setWindowIcon(QIcon (":/icon"));
      second_warning.setWindowTitle("Are you sure ?");
      second_warning.setText( "If you save you will have to save again all the adt containing the defective/missing models once you've fixed said models to correct all the issues.\n"
                              "By clicking yes you accept to bear all the consequences of your action and forfeit the right to complain to the developers about any culling and collision issues.\n\n"
                              "So... do you REALLY want to save ?"
                            );
      no = second_warning.addButton("No", QMessageBox::ButtonRole::YesRole);
      yes = second_warning.addButton("Yes", QMessageBox::ButtonRole::AcceptRole);
      second_warning.setDefaultButton(no);

      second_warning.exec();

      if (second_warning.clickedButton() == yes)
      {
        save = true;
      }
    }
  }

  if ( mode == save_mode::current 
    && save 
    && (QMessageBox::warning
          (nullptr
          , "Save current map tile only"
          , "This can cause a collision bug when placing objects between two ADT borders!\n\n"
            "We recommend you to use the normal save function rather than "
            "this one to get the collisions right."
          , QMessageBox::Save | QMessageBox::Cancel
          , QMessageBox::Cancel
          ) == QMessageBox::Cancel
       )
     )
  {
    save = false;
  }

  if (save)
  {
    makeCurrent();
    OpenGL::context::scoped_setter const _ (::gl, context());

    switch (mode)
    {
    case save_mode::current: _world->mapIndex.saveTile(TileIndex(_camera.position), _world.get()); break;
    case save_mode::changed: _world->mapIndex.saveChanged(_world.get()); break;
    case save_mode::all:     _world->mapIndex.saveall(_world.get()); break;
    }
    // write wdl, we update wdl data prior in the mapIndex saving fucntions above
    _world->horizon.save_wdl(_world.get());

    for (auto&& dbc : _dirty_dbcs)
    {
      dbc->save();
    }

    NOGGIT_ACTION_MGR->purge();
    AsyncLoader::instance->reset_object_fail();

    _main_window->statusBar()->showMessage("Map saved", 2000);

  }
  else
  {
    QMessageBox::warning
      ( nullptr
      , "Map NOT saved"
      , "The map was NOT saved, don't forget to save before leaving"
      , QMessageBox::Ok
      );
  }
}

void MapView::addHotkey(Qt::Key key, size_t modifiers, std::function<void()> function, std::function<bool()> condition)
{
  hotkeys.emplace_front (key, modifiers, function, condition);
}

void MapView::addHotkey(Qt::Key key, size_t modifiers, StringHash hotkeyName)
{
  hotkeys.emplace_front (key, modifiers
      , [=] { activeTool()->onHotkeyPress(hotkeyName); }
      , [=] { return activeTool()->hotkeyCondition(hotkeyName); }
      , [=] { activeTool()->onHotkeyRelease(hotkeyName); });
}

void MapView::unloadOpenglData()
{
  makeCurrent();
  OpenGL::context::scoped_setter const _ (::gl, context());

  ModelManager::unload_all(_context);
  WMOManager::unload_all(_context);
  TextureManager::unload_all(_context);

  for (MapTile* tile : _world->mapIndex.loaded_tiles())
  {
    tile->renderer()->unload();
    tile->Water.renderer()->unload();

    for (int i = 0; i < 16; ++i)
    {
      for (int j = 0; j < 16; ++j)
      {
        tile->getChunk(i, j)->unload();
      }
    }
  }

  _world->renderer()->unload();

  _buffers.unload();
  _gl_initialized = false;
}

QWidget* MapView::getSecondaryToolBar()
{
    return _viewport_overlay_ui->secondaryToolbarHolder;
}

QWidget* MapView::getLeftSecondaryToolbar()
{
    return _viewport_overlay_ui->leftSecondaryToolbarHolder;
}

[[nodiscard]]
Noggit::NoggitRenderContext MapView::getRenderContext()
{
  return _context;
}

[[nodiscard]]
World* MapView::getWorld() const
{
  return _world.get();
}

[[nodiscard]]
QDockWidget* MapView::getAssetBrowser()
{
  return _asset_browser_dock;
}

[[nodiscard]]
Noggit::Ui::Tools::AssetBrowser::Ui::AssetBrowserWidget* MapView::getAssetBrowserWidget()
{
  return _asset_browser;
}

glm::vec3 MapView::cursorPosition() const
{
    return _cursor_pos;
}

void MapView::cursorPosition(glm::vec3 position)
{
    _cursor_pos = position;
}

void MapView::enableGizmoBar()
{
  _viewport_overlay_ui->gizmoBar->show();
}

void MapView::disableGizmoBar()
{
  _viewport_overlay_ui->gizmoBar->hide();
}

void MapView::setDbcDirty(DBCFile* dbc)
{
  for (auto&& dirty_dbc : _dirty_dbcs)
  {
    if (dirty_dbc == dbc)
    {
      return;
    }
  }

  _dirty_dbcs.emplace_back(dbc);
}

// also called when loading world/viewport in MapView::initializeGL()
void MapView::onSettingsSave()
{
  _classic_ui = _settings->value("classicUI", false).toBool();

  OpenGL::TerrainParamsUniformBlock* params = _world->renderer()->getTerrainParamsUniformBlock();
  params->wireframe_type = _settings->value("wireframe/type", false).toBool();
  params->wireframe_radius = _settings->value("wireframe/radius", 1.5f).toFloat();
  params->wireframe_width = _settings->value ("wireframe/width", 1.f).toFloat();

  /* temporaryyyyyy */
  params->climb_value = 1.0f;

  QColor c = _settings->value("wireframe/color").value<QColor>();
  glm::vec4 wireframe_color(c.redF(), c.greenF(), c.blueF(), c.alphaF());
  params->wireframe_color = wireframe_color;

  _world->renderer()->directional_lightning = _settings->value("directional_lightning", true).toBool();
  _world->renderer()->local_lightning = _settings->value("local_lightning", true).toBool();

  // refresh rendering
  _world->renderer()->markTerrainParamsUniformBlockDirty();
  _world->renderer()->skies()->force_update();

  _world->renderer()->_view_distance = _settings->value("view_distance", 2000.f).toFloat() + TILE_RADIUS;
  _world.get()->mapIndex.setLoadingRadius(_settings->value("loading_radius", 2).toInt());
  _world.get()->mapIndex.setUnloadDistance(_settings->value("unload_dist", 5).toInt());
  _world.get()->mapIndex.setUnloadInterval(_settings->value("unload_interval", 30).toInt());

  _camera.fov(math::degrees(_settings->value("fov", 54.f).toFloat()));
  _debug_cam.fov(math::degrees(_settings->value("fov", 54.f).toFloat()));

  int _fps_limit = _settings->value("fps_limit", 60).toInt();
  int _frametime = static_cast<int>((1.f / static_cast<float>(_fps_limit)) * 1000.f);
  // _update_every_event_loop.start(_frametime);
  _update_every_event_loop.setInterval(_frametime);

  bool vsync = _settings->value("vsync", false).toBool();
  format().setSwapInterval(vsync ? 1 
                           : Noggit::Application::NoggitApplication::instance()->getConfiguration()->GraphicsConfiguration.SwapChainInternal);

  bool doAntiAliasing = _settings->value("anti_aliasing", false).toBool();
  format().setSamples(doAntiAliasing ? 4 
                      : Noggit::Application::NoggitApplication::instance()->getConfiguration()->GraphicsConfiguration.SamplesCount);

  _render_m2_aabb = _settings->value("render/m2_aabb", false).toBool();
  _render_m2_collission_bbox = _settings->value("render/m2_coll_bb", false).toBool();
  _render_wmo_aabb = _settings->value("render/wmo_aabb", false).toBool();
  _render_wmo_groups_bounds = _settings->value("render/wmo_groups_bounds", false).toBool();

  // force updating rendering
  _camera_moved_since_last_draw = true;

  auto app_config = Noggit::Application::NoggitApplication::instance()->getConfiguration();
  app_config->modern_features = _settings->value("modern_features", false).toBool();

}

void MapView::setCameraDirty()
{
  _camera_moved_since_last_draw = true;
}

[[nodiscard]]
Noggit::Ui::minimap_widget* MapView::getMinimapWidget() const
{
  return _minimap;
}

void MapView::ShowContextMenu(QPoint pos) 
{
    // QApplication::startDragDistance() is 10
    bool mouse_moved = (QApplication::startDragDistance() / 5) < (_right_click_pos - pos).manhattanLength();

    // don't show context menu if dragging mouse
    if (mouse_moved || ImGuizmo::IsUsing())
        return;

    // TODO : build the menu only once, store it and instead use setVisible ?

    QMenu* menu = new QMenu(this);

    // Undo
    QAction action_undo("Undo", this);
    menu->addAction(&action_undo);
    action_undo.setShortcut(QKeySequence::Undo);
    QObject::connect(&action_undo, &QAction::triggered, [=]()
        {
            NOGGIT_ACTION_MGR->undo();
        });
    // Redo
    QAction action_redo("Redo", this);
    menu->addAction(&action_redo);
    action_redo.setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z));
    QObject::connect(&action_redo, &QAction::triggered, [=]()
        {
            NOGGIT_ACTION_MGR->redo();
        });

    activeTool()->registerContextMenuItems(menu);

    menu->exec(mapToGlobal(pos)); // synch
    // menu->popup(mapToGlobal(pos)); // asynch, needs to be preloaded to work
}

void MapView::onApplicationStateChanged(Qt::ApplicationState state)
{
    // auto interval = _update_every_event_loop.interval();

    if (!_settings->value("background_fps_limit", true).toBool())
        return;

    int fps_limit = _settings->value("fps_limit", 60).toInt();
    int fps_calcul = (int)((1.f / (float)fps_limit) * 1000.f);

    switch (state)
    {
    case Qt::ApplicationState::ApplicationHidden:
    {
        // The application is hidden and runs in the background.
        // this isn't minimized, it's when the window is entirely hidden, should never happen on noggit
        _update_every_event_loop.setInterval(1000); // set to 1fps
        break;
    }
    case Qt::ApplicationState::ApplicationActive:
    {
        _update_every_event_loop.setInterval(fps_calcul); // normal
        break;
    }
    case Qt::ApplicationState::ApplicationInactive:
    {
        // The application is visible, but not selected to be in front.
        _update_every_event_loop.setInterval(fps_calcul * 2); // half fps if inactive
        break;
    }
    case Qt::ApplicationState::ApplicationSuspended:
    {
        // don't run updates ?
        _update_every_event_loop.setInterval(1000);
        break;
    }
    default:
        break;
    }
}
