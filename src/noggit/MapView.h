// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once

#include <math/ray.hpp>
#include <noggit/BoolToggleProperty.hpp>
#include <noggit/Camera.hpp>
#include <noggit/Selection.h>
#include <noggit/StringHash.hpp>
#include <noggit/TileIndex.hpp>
#include <noggit/tool_enums.hpp>
#include <noggit/ui/tools/ViewportGizmo/ViewportGizmo.hpp>
#include <noggit/ui/tools/ViewportManager/ViewportManager.hpp>
#include <noggit/ui/uid_fix_mode.hpp>
#include <opengl/scoped.hpp>

#include <QtCore/QElapsedTimer>
#include <QtCore/QTimer>

#include <array>
#include <cstddef>
#include <cstdint>
#include <forward_list>
#include <vector>


class DBCFile;
class World;
struct ImGuiContext;

class QSettings;
class QDockWidget;
class QLabel;
class QWidgetAction;
class QOpenGLContext;

namespace Noggit::Ui::Windows
{
    class NoggitWindow;
}

namespace Noggit
{
  class Tool;
  class TabletManager;

  namespace Database
  {
    class SchemaModel;
    class SpawnSceneCache;
    class WorldDatabaseConnection;
    struct ConnectionConfig;
    struct SpawnRef;
  }

  namespace Project
  {
    class NoggitProject;
  }

  namespace Ui::Tools::ViewToolbar::Ui
  {
    class ViewToolbar;
  }

  namespace Ui::Tools
  {
    class ToolPanel;

    namespace MissingObjects
    {
      class MissingObjectsPanel;
    }

    namespace AssetBrowser::Ui
    {
      class AssetBrowserWidget;
    }
  }
	
  namespace Ui
  {
    class DatabaseSpawnPanel;
    class detail_infos;
    class help;
    class minimap_widget;
    class toolbar;
  }
}

namespace OpenGL
{
  class texture;
}

namespace Ui {
  class MapViewOverlay;
}

enum class save_mode
{
  current,
  changed,
  all
};

class MapView : public Noggit::Ui::Tools::ViewportManager::Viewport
{
  Q_OBJECT
public:
  bool _mod_alt_down = false;
  bool _mod_ctrl_down = false;
  bool _mod_shift_down = false;
  bool _mod_space_down = false;
  bool _mod_num_down = false;

  bool  leftMouse = false;
  bool  leftClicked = false;
  bool  rightMouse = false;

  std::unique_ptr<World> _world;
  Noggit::Camera _camera;

private:

  float _2d_zoom = 1.f;
  float moving, strafing, updown, mousedir, turn, lookat;
  CursorType _cursorType;
  glm::vec3 _cursor_pos;
  QPoint _drag_start_pos;
  QPoint _right_click_pos;
  float _cursorRotation;
  bool look, freelook;
  bool ui_hidden = false;

  Noggit::Camera _debug_cam;
  Noggit::BoolToggleProperty _debug_cam_mode = { false };
  Noggit::BoolToggleProperty _fps_mode = { false };
  Noggit::BoolToggleProperty _camera_collision = { false };

  bool _camera_moved_since_last_draw = true;

  std::array<Qt::Key, 6> _inputs = {Qt::Key_W, Qt::Key_S, Qt::Key_D, Qt::Key_A, Qt::Key_Q, Qt::Key_E};
  void checkInputsSettings();

public:
  Noggit::BoolToggleProperty _draw_vertex_color = {true};
  Noggit::BoolToggleProperty _draw_baked_shadows = { false };
  Noggit::BoolToggleProperty _draw_climb = {false};
  Noggit::BoolToggleProperty _draw_contour = {false};
  Noggit::BoolToggleProperty _draw_mfbo = {false};
  Noggit::BoolToggleProperty _draw_wireframe = {false};
  Noggit::BoolToggleProperty _draw_lines = {false};
  Noggit::BoolToggleProperty _draw_terrain = {true};
  Noggit::BoolToggleProperty _draw_wmo = {true};
  Noggit::BoolToggleProperty _draw_water = {true};
  Noggit::BoolToggleProperty _draw_wmo_doodads = {true};
  Noggit::BoolToggleProperty _draw_wmo_exterior = { true };
  Noggit::BoolToggleProperty _draw_models = {true};
  Noggit::BoolToggleProperty _draw_model_animations = {true};
  Noggit::BoolToggleProperty _draw_hole_lines = {false};
  Noggit::BoolToggleProperty _draw_models_with_box = {false};
  Noggit::BoolToggleProperty _draw_fog = {false};
  Noggit::BoolToggleProperty _draw_sky = { true };
  Noggit::BoolToggleProperty _draw_skybox = { true };
  Noggit::BoolToggleProperty _draw_hidden_models = {false};
  Noggit::BoolToggleProperty _draw_occlusion_boxes = {false};
  // Off by default: an overlay of server-side data has to be asked for, never assumed, and
  // nothing is loaded until "Load database spawns" is used.
  Noggit::BoolToggleProperty _draw_db_spawns = {false};
  // Noggit::BoolToggleProperty _game_mode_camera = { false };
  Noggit::BoolToggleProperty _draw_lights_zones = { false };
  Noggit::BoolToggleProperty _show_detail_info_window = { false };
  Noggit::BoolToggleProperty _show_minimap_window = { false };
private:

  void update_cursor_pos();

  display_mode _display_mode;

  void draw_map();

  void createGUI();

public:
  // Public because the spawn panel drives both, and it is a separate widget rather than a friend:
  // the panel is the buttons, MapView owns the world and the connection, and that seam is worth
  // keeping visible.

  // Queries the world database and rebuilds the spawn overlay.
  //
  // Driven by an explicit menu action, never by tile streaming. The query runs here, on the main
  // thread, inside the Qt handler -- which is the point: the render path must never issue a query
  // or walk a DBC inside a frame, and WorldDatabaseConnection holds a single unsynchronised
  // connection that must not be touched from the loader threads.
  //
  // `all_loaded_tiles` false loads only the tile under the camera, which is the normal case and
  // what the edit workflow actually needs. True loads every loaded tile -- up to a 5x5 block by
  // default, more if the camera has moved recently -- and runs a pre-flight COUNT first, because
  // against a populated world database that can be many thousands of spawns, each one queueing an
  // asynchronous model load.
  //
  // `interactive` decides how the outcome is delivered, not what it is: true raises a dialog, as
  // the menu wants; false returns it and shows nothing, as a caller with no human in front of it
  // needs -- code left waiting on a modal nobody will click is a hang. Either way the same line
  // comes back, prefixed `OK ` or `ERR `, so the two paths cannot describe an outcome differently.
  //
  // `force` skips the confirmation threshold. Non-interactive callers are refused above it rather
  // than silently allowed, because that threshold exists for exactly the case a script hits.
  std::string loadDatabaseSpawns(bool all_loaded_tiles, bool interactive = true, bool force = false);

  // Loads database spawns for an explicit set of ADT tiles, replacing whatever the overlay holds.
  //
  // The whole of loadDatabaseSpawns below the tile-set decision, callable with a tile set nobody
  // derived from the camera: the connection, the schema introspection, the unsaved-changes
  // warning, the pre-flight COUNT and its threshold, the lazy construction of the scene cache and
  // -- the one that terminates the process when it is left out -- the makeCurrent() plus
  // OpenGL::context::scoped_setter around clear() and setTile(). loadDatabaseSpawns is now a thin
  // wrapper that builds a tile list and calls this, so there is exactly one copy of all of it.
  //
  // `tiles` are ::TileIndex, i.e. (x, z) in ADT-filename order, the frame every Noggit caller
  // already has. The transposition to the database layer's (x, y) happens inside, through
  // fromAdtFileIndex. Never hand this a Noggit::Database::TileIndex converted by hand.
  //
  // Deliberately does NOT require the camera to be over a loaded tile, or those tiles to be
  // streamed in at all: a spawn overlay is read from the database by coordinate and has no
  // dependency on ADT geometry being resident. That gate belongs to the camera-relative path only.
  //
  // Same `OK `/`ERR ` line and same meaning for `interactive` and `force` as loadDatabaseSpawns.
  std::string loadDatabaseSpawnsForTiles(std::vector<::TileIndex> const& tiles
                                        , bool interactive = true
                                        , bool force = false);

  // Emits every moved spawn as a reviewable .sql changeset, and optionally applies it.
  //
  // "Save to database" cannot mean writing to the connected server, and that is a rule rather
  // than a limitation: HARD RULE 2 requires every edit to become a DELETE-then-INSERT changeset a
  // human can read before it touches anything, and HARD RULE 1 permits writes only against the
  // configured dev schema. So this writes a file, and `apply` runs it against that dev schema and
  // nowhere else -- WorldDatabaseConnection refuses to construct write-capable against any other.
  //
  // Returns the same `OK `/`ERR ` line shape as loadDatabaseSpawns, for the same reason.
  std::string saveDatabaseChanges(bool apply_to_dev, bool interactive = true);

  // Places a new creature or gameobject at `position`, in Noggit's frame.
  //
  // `entry` names a row of creature_template / gameobject_template. That row is what supplies
  // the display id, and a display id is the only thing that makes the spawn visible, so this
  // opens a READ_ONLY connection and looks the template up rather than trusting the number. An
  // entry that does not exist, or one whose template names no model, is refused with the reason
  // -- a spawn that cannot be drawn cannot be picked, dragged or checked either, and would exist
  // only as a line in a changeset the user had no way to review.
  //
  // Nothing is written anywhere. The spawn joins the scene cache as a pending creation and
  // becomes SQL only when the user saves, with its guid allocated on the server at apply time --
  // see ChangesetBuilder::addNewCreature.
  //
  // Same `OK `/`ERR ` line and same meaning for `interactive` as loadDatabaseSpawns.
  std::string createDatabaseSpawn( bool creature
                                 , std::uint32_t entry
                                 , glm::vec3 const& position
                                 , bool interactive = true
                                 );

  // Marks a loaded spawn for deletion. It leaves the viewport immediately; the DELETE reaches a
  // file only when the user saves, and only for a guid the database actually issued.
  std::string deleteDatabaseSpawn(Noggit::Database::SpawnRef const& spawn
                                 , bool interactive = true);

  // Undoes every unsaved spawn change: moves, placements and deletions.
  //
  // Here rather than in the panel because it destroys model references, and that needs a current
  // OpenGL context -- see SpawnSceneCache::discardPending. The panel owns no context.
  std::string discardDatabaseSpawnChanges();

private:

  // The one exit shape the database spawn actions report through.
  //
  // A dialog only when a human asked: `interactive` false means nobody is at the window, and code
  // left waiting on a modal nobody will click is a hang. Either way the same line comes back,
  // prefixed `OK ` or `ERR `, so the menu and the panel cannot describe an outcome differently.
  std::string reportDatabaseSpawnOutcome(std::string const& message
                                        , bool is_error
                                        , bool interactive);

  // The introspected schema for `connection`, read once and then reused.
  //
  // Every database action here needs a SchemaModel, because HARD RULE 3 forbids hardcoding a
  // column name or ordinal: what the schema is asked is not "is this TrinityCore" but "does this
  // table have this column, and where". Building one means SELECTing every row of
  // information_schema.columns for the whole schema -- roughly a hundred thousand rows against a
  // populated TDB world database -- and it was being rebuilt per call.
  //
  // For loading and saving that is once per deliberate action and nobody notices. For
  // createDatabaseSpawn it is once per CLICK, reached synchronously from mousePressEvent, so
  // every attempt to place a spawn opened a connection and re-read the entire schema on the GUI
  // thread before anything appeared. That is what this cache is for.
  //
  // What is cached is the INTROSPECTED RESULT, never an assumed layout -- the difference matters.
  // The answer to "does creature have wander_distance" is still measured; it is measured once.
  //
  // `config` is the settings the connection was opened with, and it is what the cache is keyed
  // on: point Settings at a different server, port, user or schema and the fingerprint changes,
  // so the next call re-reads rather than answering questions about the schema it used to be
  // looking at. `refresh` forces a re-read regardless, which the load action passes -- it is the
  // "go and look at the database" action, it is not on any hot path, and it is how a schema that
  // changed under a running editor (a TDB update applied in another window) gets picked up
  // without a restart.
  //
  // The reference is valid until the next call with a different fingerprint or with `refresh`.
  // Callers use it within one method against one connection, so it cannot be invalidated
  // underneath them; do not store it in a member.
  //
  // Guarded, unlike the cache members below, because WorldDatabaseConnection.cpp and
  // SchemaIntrospector.cpp are excluded from the build entirely when Connector/C++ is absent
  // (see the LIST(FILTER ...) in CMakeLists.txt). The model itself is pure and always compiles;
  // only reading one off a server needs the connector.
#ifdef USE_MYSQL_UID_STORAGE
  Noggit::Database::SchemaModel const& databaseSchemaFor
    ( Noggit::Database::WorldDatabaseConnection const& connection
    , Noggit::Database::ConnectionConfig const& config
    , bool refresh = false
    );
#endif

  // Built lazily by loadDatabaseSpawns, so a session that never asks pays nothing and a build
  // with no database configured never constructs either. Held by pointer and forward-declared to
  // keep the database headers out of MapView.h.
  std::unique_ptr<Noggit::Database::SpawnSceneCache> _db_spawn_scene;

  // The cached schema model and what it was read from. Null until the first database action.
  //
  // Held by pointer for the same reason _db_spawn_scene is: it keeps SchemaModel.hpp out of this
  // header. The fingerprint is host, port, user and schema joined -- everything that decides
  // WHICH schema was measured. The password is deliberately not part of it: changing it cannot
  // change what the columns are.
  std::unique_ptr<Noggit::Database::SchemaModel> _db_schema;
  std::string _db_schema_fingerprint;

  // Owned by its dock. Null until the dock is built, which is why every use is guarded.
  Noggit::Ui::DatabaseSpawnPanel* _db_spawn_panel = nullptr;

  // The missing-placement report and its dock. Both null until setupViewMenu has run, and both
  // owned by the dock/main window rather than by MapView -- the dock is deleteLater'd from
  // MapView's destroyed signal, exactly as the spawn dock is.
  Noggit::Ui::Tools::MissingObjects::MissingObjectsPanel* _missing_objects_panel = nullptr;
  QDockWidget* _missing_objects_dock = nullptr;

  // Does the object transform gizmo have something to draw for World's current selection?
  //
  // Not `_world->has_selection()`, which is the trap: draw_map calls doSelection(true) on every
  // frame the selection is empty and _locked_cursor_mode is off, and that stores the MapChunk under
  // the cursor. One mouse movement over terrain therefore satisfies has_selection() forever, and
  // nothing clears it per frame -- so anything gated on it is unreachable in normal use while the
  // object gizmo it was meant to defer to is not drawn at all (handleTransformGizmo returns early
  // for a lone non-object entry).
  //
  // Answered by ViewportGizmo::drawsForSelection rather than re-derived here, so this and the
  // gizmo cannot disagree about which frames the object gizmo owns.
  //
  // Says nothing about _gizmo_on -- it is about the selection. Use gizmoIsDrawn for "is a gizmo
  // on screen right now".
  [[nodiscard]]
  bool objectGizmoHasTarget() const;

  // Is any gizmo -- object or spawn -- actually drawing handles this frame?
  //
  // The term every "the gizmo gets this click" guard needs and one of them was missing.
  // ImGuizmo::IsOver() recomputes hover from gContext.mMVP and mScreenSquareMin/Max, which only
  // ComputeContext inside Manipulate ever writes. On a frame where no Manipulate runs those are
  // stale from the last object manipulated, so IsOver() keeps answering true for a screen region
  // whose gizmo is long gone -- swallowing clicks that land in it. Asking whether a gizmo is drawn
  // at all is what makes the stale geometry unreachable.
  [[nodiscard]]
  bool gizmoIsDrawn() const;

  // The spawn the axis gizmo should be attached to this frame, or an invalid ref for none.
  //
  // How the spawn gizmo and the object editor's gizmo are kept apart: this returns nothing while
  // the object gizmo has a target, so at most one of the two is ever drawn in a frame. They share an
  // ImGuizmo ID (there is only one gContext and IsUsing() ignores the ID anyway), and mutual
  // exclusion is what makes that safe -- two gizmos live in the same frame would both respond to
  // the same drag.
  //
  // Returns a value, never a pointer. loadDatabaseSpawns clears the cache and destroys every
  // SpawnSceneEntry in it, so anything held across frames has to be a SpawnRef.
  [[nodiscard]]
  Noggit::Database::SpawnRef spawnGizmoTarget() const;

  // Draws the spawn gizmo and applies one frame of drag through SpawnSceneCache.
  //
  // Only valid inside a begun ImGui frame -- see paintGL, which is the only caller.
  void handleSpawnGizmo(Noggit::Database::SpawnRef const& spawn);

  // Set while a spawn gizmo drag is in progress.
  //
  // The panel is rebuilt once on release rather than on every frame of the drag: refresh() clears
  // and repopulates the QListWidget, which moves the row out from under the cursor and churns the
  // selection through currentRowChanged sixty times a second.
  bool _spawn_gizmo_dragging = false;

  QWidgetAction* createTextSeparator(const QString& text);

  float mTimespeed;

  void ResetSelectedObjectRotation();

  QPointF _last_mouse_pos;

  glm::vec3 objMove;

  std::vector<selection_type> lastSelected;

  // Vars for the ground editing toggle mode store the status of some
  // view settings when the ground editing mode is switched on to
  // restore them if switch back again
  std::shared_ptr<Noggit::Project::NoggitProject> _project;
  bool  alloff = true;
  bool  alloff_models = false;
  bool  alloff_doodads = false;
  bool  alloff_contour = false;
  bool  alloff_wmo = false;
  bool  alloff_detailselect = false;
  bool  alloff_fog = false;
  bool  alloff_terrain = false;
  bool  alloff_climb = false;
  bool  alloff_vertex_color = false;
  bool  alloff_baked_shadows = false;

  bool _render_m2_aabb = false;
  bool _render_m2_collission_bbox = false;
  bool _render_wmo_aabb = false;
  bool _render_wmo_groups_bounds = false;

  bool _classic_ui = false;

  editing_mode terrainMode = editing_mode::ground;
  editing_mode saveterrainMode = terrainMode;

  bool _uid_duplicate_warning_shown = false;
  bool _force_uid_check = false;
  bool _uid_fix_failed = false;
  void on_uid_fix_fail();

  uid_fix_mode _uid_fix;
  bool _from_bookmark;

  Noggit::Ui::toolbar* _toolbar;
  Noggit::Ui::Tools::ViewToolbar::Ui::ViewToolbar* _view_toolbar;
  Noggit::Ui::Tools::ViewToolbar::Ui::ViewToolbar* _secondary_toolbar;
  Noggit::Ui::Tools::ViewToolbar::Ui::ViewToolbar* _left_sec_toolbar;

  void save(save_mode mode);

  QSettings* _settings; // expensive, don't access it on main loop
  Noggit::Ui::Tools::ViewportGizmo::ViewportGizmo _transform_gizmo;
  ImGuiContext* _imgui_context;

signals:
  void uid_fix_failed();
  void resized();
  void saved();
  void updateProgress(int value);
  void selectionUpdated(std::vector<selection_type>& selection);
  void menuToggleChanged(bool value);
  void rotationChanged();
  void trySetBrushTexture(QImage* image, QWidget* sender);
public slots:
  void on_exit_prompt();
  void ShowContextMenu(QPoint pos);
  void onApplicationStateChanged(Qt::ApplicationState state);

public:
  glm::vec4 cursor_color;

  MapView ( math::degrees ah0
          , math::degrees av0
          , glm::vec3 camera_pos
          , Noggit::Ui::Windows::NoggitWindow*
          , std::shared_ptr<Noggit::Project::NoggitProject> Project
          , std::unique_ptr<World>
          , uid_fix_mode uid_fix = uid_fix_mode::none
          , bool from_bookmark = false
          );
  ~MapView();

  // Path of the texture currently selected in the Texturing tool, or empty when none is.
  [[nodiscard]]
  std::string selectedTexturePath() const;

  [[nodiscard]]
  glm::vec3 cameraPosition() const;

  // The loaded spawn scene, or null when nothing has been loaded. For the spawn panel.
  [[nodiscard]]
  Noggit::Database::SpawnSceneCache* databaseSpawns() const;

  // Terrain textures actually painted in scope, mapped to how many layers use each.
  //
  // Lives here rather than in the ground effect panel because more than one caller needs it --
  // the ground effect panel and the auto-texture dialog both offer only textures that exist.
  // One scan, one answer.
  [[nodiscard]]
  std::map<std::string, std::size_t> terrainTexturesInScope(bool all_loaded_tiles) const;

  // Request a repaint after the spawn overlay changed.
  //
  // The editor only paints when something asks it to, so moving or rotating a spawn from a panel
  // -- which is not an input event -- leaves the viewport showing the old position until
  // something else happens to trigger a frame.
  void markSpawnOverlayDirty();

  // guid of the database spawn under the cursor, or 0.
  //
  // Nearest-hit across every loaded spawn rather than first-hit: overlapping creatures are
  // normal in a camp, and picking whichever happened to be iterated first would select one
  // the user cannot see.
  [[nodiscard]]
  Noggit::Database::SpawnRef pickDatabaseSpawn();

  // Point the camera at a loaded spawn. False when that guid is not loaded.
  //
  // The one place a spawn is framed, so nothing that focuses on one can drift into framing it
  // differently.
  bool focusOnSpawn(Noggit::Database::SpawnRef const& spawn, float distance = 12.0f);

  // Point the camera at an arbitrary world position, framing it the way focusOnSpawn frames a
  // spawn -- the body of that function, extracted, so the two cannot drift into framing things
  // differently.
  //
  // Not move_camera_with_auto_height (MapView.cpp:4424), which the minimap uses: that one
  // discards the Y it is given and snaps to terrain + 50. For a missing placement the exact Y is
  // known and is often the whole point -- a broken object on a bridge, inside a building or
  // under the terrain is precisely where terrain-snapping puts the camera in the wrong place.
  //
  // `load_tile` force-loads the ADT the point sits in first, for rows whose tile the world has
  // since streamed out. False when the point is outside the 64x64 grid.
  bool focusOnPoint(glm::vec3 const& target, float distance = 12.0f, bool load_tile = true);

  void tick (float dt);
  void change_selected_wmo_nameset(int set);
  void change_selected_wmo_doodadset(int set);
  auto setBrushTexture(QImage const* img) -> void;
  Noggit::Camera* getCamera();;
  void onSettingsSave();
  void setCameraDirty();;

  [[nodiscard]]
  Noggit::Ui::minimap_widget* getMinimapWidget() const;

  void set_editing_mode (editing_mode);
  editing_mode get_editing_mode() const;;

  [[nodiscard]]
  QWidget *getSecondaryToolBar();

  [[nodiscard]]
  QWidget *getLeftSecondaryToolbar();

  [[nodiscard]]
  Noggit::NoggitRenderContext getRenderContext();;

  [[nodiscard]]
  World* getWorld() const;;

  [[nodiscard]]
  QDockWidget* getAssetBrowser();;

  [[nodiscard]]
  Noggit::Ui::Tools::AssetBrowser::Ui::AssetBrowserWidget* getAssetBrowserWidget();;

  glm::vec3 cursorPosition() const;
  void cursorPosition(glm::vec3 position);

  void enableGizmoBar();
  void disableGizmoBar();

  void setDbcDirty(DBCFile* dbc);

private:
  enum Modifier
  {
    MOD_shift = 0x01,
    MOD_ctrl = 0x02,
    MOD_alt = 0x04,
    MOD_meta = 0x08,
    MOD_space = 0x10,
    MOD_num = 0x20,
    MOD_none = 0x00
  };
  struct HotKey
  {
    Qt::Key key;
    size_t modifiers;
    std::function<void()> onPress;
    std::function<void()> onRelease;
    std::function<bool()> condition;
    HotKey (Qt::Key k, size_t m, std::function<void()> f, std::function<bool()> c, std::function<void()> r = []{})
      : key (k), modifiers (m), onPress(f), onRelease{r}, condition (c) {}
  };

  std::forward_list<HotKey> hotkeys;

  void addHotkey(Qt::Key key, size_t modifiers, std::function<void()> function, std::function<bool()> condition = [] { return true; });
  void addHotkey(Qt::Key key, size_t modifiers, StringHash hotkeyName);

  QElapsedTimer _startup_time;
  qreal _last_update = 0.f;
  std::list<qreal> _last_frame_durations;

  float _last_fps_update = 0.f;

  QTimer _update_every_event_loop;

  QOpenGLContext* _last_opengl_context;

  virtual void tabletEvent(QTabletEvent* event) override;
  virtual void initializeGL() override;
  virtual void paintGL() override;
  virtual void resizeGL (int w, int h) override;
  virtual void mouseMoveEvent (QMouseEvent*) override;
  virtual void mousePressEvent (QMouseEvent*) override;
  virtual void mouseReleaseEvent (QMouseEvent*) override;
  virtual void wheelEvent (QWheelEvent*) override;
  virtual void keyReleaseEvent (QKeyEvent*) override;
  virtual void keyPressEvent (QKeyEvent*) override;
  virtual void focusOutEvent (QFocusEvent*) override;
  virtual void enterEvent(QEvent*) override;

  Noggit::Ui::Windows::NoggitWindow* _main_window;

  glm::vec4 normalized_device_coords (int x, int y) const;

  Noggit::TabletManager* _tablet_manager;

  QLabel* _status_position;
  QLabel* _status_selection;
  QLabel* _status_area;
  QLabel* _status_time;
  QLabel* _status_fps;
  QLabel* _status_culling;
  QLabel* _status_database;

  //! The area name in the status bar, kept between frames. tick() called
  //! AreaDB::getAreaFullName every frame, and that function walks every record in AreaTable.dbc
  //! twice -- once for the area, once for its parent region -- because DBCFile::getByID is a
  //! linear scan with no index (DBCFile.cpp:144). The answer only depends on the area id, and the
  //! DBC is immutable once loaded, so re-deriving it while the camera sits inside one area is
  //! pure waste. Worse on a custom map: an area id that is not in AreaTable.dbc makes getByID log
  //! and then THROW, so the old code paid a LogDebug line and a C++ exception every frame.
  //! _status_area_known distinguishes "not looked up yet" from any id value, including the
  //! 0xFFFFFFFF getAreaID returns for no chunk.
  unsigned int _status_area_id = 0;
  bool _status_area_known = false;
  QString _status_area_name;

  //! Rate limit for the detail-info panel while a brush stroke is held. See the call site in
  //! tick(). _detail_infos_stale records that a frame's update was skipped, so the panel is
  //! always brought up to date once on the first frame after the stroke ends -- a throttle that
  //! could drop the last update would leave the panel showing values from mid-stroke.
  QElapsedTimer _detail_infos_clock;
  bool _detail_infos_stale = false;

  Noggit::BoolToggleProperty _locked_cursor_mode = {false};
  Noggit::BoolToggleProperty _rotate_doodads_along_doodads = { false };
  Noggit::BoolToggleProperty _rotate_doodads_along_wmos = { false };

  Noggit::BoolToggleProperty _show_node_editor = {false};
  Noggit::BoolToggleProperty _show_minimap_borders = {true};
  Noggit::BoolToggleProperty _show_minimap_skies = {false};
  Noggit::BoolToggleProperty _show_keybindings_window = {false};
  Noggit::BoolToggleProperty _showStampPalette{false};

  Noggit::Ui::minimap_widget* _minimap;
  QDockWidget* _minimap_dock;

  void setToolPropertyWidgetVisibility(editing_mode mode);

  void unloadOpenglData() override;

  Noggit::Ui::help* _keybindings;
  Noggit::Ui::detail_infos* guidetailInfos;

  OpenGL::texture* const _texBrush;

  Noggit::Ui::Tools::AssetBrowser::Ui::AssetBrowserWidget* _asset_browser = nullptr;

  QDockWidget* _asset_browser_dock;
  QDockWidget* _node_editor_dock;
  QDockWidget* _detail_infos_dock;

  Noggit::Ui::Tools::ToolPanel* _tool_panel_dock;

  ::Ui::MapViewOverlay* _viewport_overlay_ui;
  ImGuizmo::MODE _gizmo_mode = ImGuizmo::MODE::WORLD;
  ImGuizmo::OPERATION _gizmo_operation = ImGuizmo::OPERATION::TRANSLATE;
  Noggit::BoolToggleProperty _gizmo_on = {true};

  bool _change_operation_mode = false;
  void updateGizmoOverlay(ImGuizmo::OPERATION operation);

  bool _gl_initialized = false;
  bool _destroying = false;
  bool _needs_redraw = false;
  bool _unload_tiles = true;

  OpenGL::Scoped::deferred_upload_buffers<2> _buffers;

  glm::mat4x4 _model_view;
  glm::mat4x4 _projection;

  std::vector<DBCFile*> _dirty_dbcs;

public:

private:

  void setupViewportOverlay();
  void setupNodeEditor();
  void setupAssetBrowser();
  void setupDetailInfos();
  void updateDetailInfos();
  void setupToolbars();
  void setupKeybindingsGui();
  void setupMinimap();
  void setupFileMenu();
  void setupEditMenu();
  void setupAssistMenu();
  void setupViewMenu();
  void setupToolsMenu();
  void setupHelpMenu();
  void setupHotkeys();
  void setupClientMenu();
  void setupMainToolbar();

  QWidget* _overlay_widget;

  std::vector<std::unique_ptr<Noggit::Tool>> _tools;
  size_t _activeToolIndex = 0;

  std::unique_ptr<Noggit::Tool>& activeTool();
  void activeTool(editing_mode newTool);

  public:
  [[nodiscard]]
  Noggit::Ui::Tools::ViewToolbar::Ui::ViewToolbar* getLeftSecondaryViewToolbar();

  [[nodiscard]]
  QSettings* settings();

  [[nodiscard]]
  Noggit::Ui::Windows::NoggitWindow* mainWindow();

  [[nodiscard]]
  bool isUiHidden() const;

  [[nodiscard]]
  bool drawAdtGrid() const;
  [[nodiscard]]
  bool drawHoleGrid() const;

  void invalidate();

  void selectObjects(std::array<glm::vec2, 2> selection_box, float depth);
  void doSelection(bool selectTerrainOnly, bool mouseMove = false);
  void DeleteSelectedObjects();
  void snap_selected_models_to_the_ground();

  [[nodiscard]]
  bool isRotatingCamera() const;

  [[nodiscard]]
  float aspect_ratio() const;

  [[nodiscard]]
  math::ray intersect_ray() const;

  [[nodiscard]]
  selection_result intersect_result(bool terrain_only);

  [[nodiscard]]
  std::shared_ptr<Noggit::Project::NoggitProject>& project();

  [[nodiscard]]
  float timeSpeed() const;

  [[nodiscard]]
  glm::mat4x4 model_view(bool use_debug_cam = false) const;

  [[nodiscard]]
  glm::mat4x4 projection() const;

  void move_camera_with_auto_height(glm::vec3 const&);
};
