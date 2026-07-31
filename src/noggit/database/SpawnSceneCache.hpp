// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DATABASE_SPAWNSCENECACHE_HPP
#define NOGGIT_DATABASE_SPAWNSCENECACHE_HPP

#include <noggit/database/DisplayResolver.hpp>
#include <noggit/database/SpawnTypes.hpp>
#include <noggit/ContextObject.hpp>
#include <noggit/ModelInstance.h>
#include <noggit/scoped_blp_texture_reference.hpp>
#include <noggit/TileIndex.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Noggit::Database
{
  // The glue that turns database rows into something the renderer can see.
  //
  // Everything else under src/noggit/database is either pure or depends on nothing heavier than
  // DBC.h, which is what keeps the logic layer in a test target that needs no Qt and no client
  // install. This file is the deliberate exception: it names ModelInstance and therefore pulls in
  // the whole scene graph, so it is NOT added to tests/CMakeLists.txt. What can be tested without
  // a toolchain -- the coordinate frame, the yaw, the display-id rule, the path rewrite -- was
  // pulled out into SpawnPlacement, SpawnDisplay and ModelPathFixup precisely so that this file
  // could stay thin enough not to need testing of its own.
  //
  // ModelInstance.h is included rather than forward-declared, against the usual guideline,
  // because SpawnSceneEntry holds instances by unique_ptr inside a std::vector: keeping the type
  // incomplete would mean hand-writing out-of-line destructors for both types to buy nothing.

  enum class SpawnKind : std::uint8_t
  {
    CREATURE = 0,
    GAMEOBJECT = 1
  };

  // One drawable spawn.
  //
  // The instance is held by unique_ptr rather than by value so its address is stable: M2's
  // picking will hand ModelInstance* around, and a vector reallocation must not invalidate it.
  //
  // uid stays 0 and this instance is never registered with world_model_instances_storage or
  // added to MapTile::object_instances. That is what keeps the "DB spawns never enter MDDF/MODF"
  // rule structural rather than a convention someone has to remember -- the save path walks the
  // tile's own instance index, which never sees these.
  struct SpawnSceneEntry
  {
    std::unique_ptr<ModelInstance> instance;
    SpawnKind kind = SpawnKind::CREATURE;
    std::uint32_t guid = 0;
    std::uint32_t display_id = 0;

    // The row this was built from, kept so an edit can be written back.
    //
    // Only the one matching `kind` is meaningful. They are held by value rather than re-queried
    // at save time on purpose: the changeset has to describe the row as it was read, with every
    // column the editor never touches carried through unchanged. Re-reading at save would race
    // with anything else editing the database, and would silently launder a concurrent change
    // into a diff the user never made.
    CreatureSpawn creature;
    GameObjectSpawn gameobject;

    // Set once the user moves it. Only dirty entries reach the changeset.
    bool dirty = false;

    // Replaceable-texture skins for this spawn's display id -- see ResolvedModel::skins. Carried
    // per entry rather than looked up at draw time so the render path touches no DBC.
    //
    // Note these belong to the DISPLAY ID, not to the model: one wolf model serves many wolves of
    // different colours. That is why the draw path groups by (model, display id) rather than by
    // model alone, as the ADT path does.
    std::vector<std::pair<int, std::string>> skins;

    // The same skins as live references, held for as long as the spawn is loaded.
    //
    // > [!warning] These must be owned here, not created at draw time
    // > A scoped_blp_texture_reference is refcounted, and BLPs load asynchronously. Constructing
    // > one inside the draw pass and letting it die when the frame's texture swap is undone drops
    // > the count to zero every frame: the load is queued, the frame draws before it finishes, the
    // > texture is unloaded, and the next frame starts over. The result is a model that renders
    // > black forever while every diagnostic says the skin resolved correctly -- which is exactly
    // > what happened.
    // >
    // > Holding them here means the load happens once and the reference outlives it.
    std::vector<std::pair<int, scoped_blp_texture_reference>> skin_textures;
  };

  // Why spawns in a tile were not drawn.
  //
  // Kept rather than discarded because M1's brief requires the unrenderable cases to be handled
  // "without breaking", and silence is the one way of handling them that cannot be told apart
  // from a tile that genuinely has no spawns. A count makes an empty overlay explainable.
  struct SpawnSkipCounts
  {
    // Neither the spawn nor its template named a model. Normal and common.
    std::size_t no_display_id = 0;

    // GAMEOBJECT_TYPE that carries no model whatever its displayId says -- traps, transports,
    // cameras. See SpawnDisplay::typeHasRenderableModel.
    std::size_t non_renderable_type = 0;

    // A display id that resolved to nothing: a DBC row absent for this client, or a name this
    // loader does not handle. The commonest case by far is a gameobject whose
    // GameObjectDisplayInfo.ModelName is a .wmo, which is a different loader entirely and which
    // ModelPathFixup rejects on purpose rather than mangling into an .m2 key.
    std::size_t unresolved_model = 0;

    std::size_t total() const
    {
      return no_display_id + non_renderable_type + unresolved_model;
    }
  };

  // Everything drawable for one ADT tile, plus what was left out.
  struct TileSpawnScene
  {
    std::vector<SpawnSceneEntry> entries;
    SpawnSkipCounts skipped;
  };

  // Per-tile store of drawable spawns, keyed in ADT filename order.
  //
  // Populated by an explicit user action, never during tile streaming, and read during draw. The
  // render path only ever looks things up here; no query and no DBC scan happens inside a frame.
  //
  // NOT thread-safe. It owns scoped_model_references and it is read by the render thread, and
  // DisplayResolver -- which it calls into while building -- is not synchronised either. Use it
  // from the main thread only.
  //
  // > [!warning] An OpenGL context must be current when this object is mutated or destroyed
  // > Every entry owns a scoped_model_reference. Releasing the last reference to a Model runs
  // > Model::~Model -> ModelRender::~ModelRender, which destroys OpenGL vertex arrays, and
  // > OpenGL::Scoped's destructor throws from verify_context_and_check_for_gl_errors when no
  // > context is bound (context.inl:47). A throw from a destructor terminates the process, so
  // > this is a hard crash rather than a diagnosable error, and it does NOT reproduce on the
  // > first load -- only once there is something to release.
  // >
  // > So setTile(), clear() and the destructor all require a bound context. Callers on the Qt
  // > side must wrap them in `makeCurrent()` plus `OpenGL::context::scoped_setter`, as
  // > MapView::loadDatabaseSpawns and ~MapView both do. Nothing in the type system enforces
  // > this; it is the same unguarded hazard the "opengl context related crash" comment in
  // > ~MapView refers to.
  class SpawnSceneCache
  {
    public:
      explicit SpawnSceneCache(Noggit::NoggitRenderContext context);

      // Builds the drawable form of one tile's spawns, replacing anything already held for that
      // tile.
      //
      // The ADT key is derived here from `spawns.tile` rather than taken as a parameter, so a
      // caller cannot transpose it. Database::TileIndex and Noggit's ::TileIndex disagree about
      // which axis comes first, and assigning one to the other field-by-field loads a tile about
      // 9.6 km away -- see TileCoordinates.hpp on toAdtFileIndex.
      void setTile(TileSpawns const& spawns);

      // Drops the DBC resolution cache but keeps the built scenes. Needed after the DBCs are
      // reopened for a different client install, which invalidates every resolved path.
      void clearResolverCache() { _resolver.clearCache(); }

      // Null when nothing has been loaded for this tile.
      TileSpawnScene const* tile(::TileIndex const& adt_tile) const;

      // Moves a loaded spawn to a new Noggit-frame position and marks it dirty.
      //
      // Converts back to server coordinates through SpawnPlacement::serverPositionFor, so the
      // stored row stays the authority on where the spawn is and the round trip goes through the
      // one tested seam rather than being open-coded at a UI call site.
      bool moveTo(std::uint32_t guid, glm::vec3 const& position);

      // Sets a spawn's facing, in server radians, and marks it dirty.
      //
      // For a gameobject this also rewrites rotation0..3. The core reads `orientation` but the
      // client renders the quaternion, so writing one without the other produces a spawn that
      // faces two different ways depending on who is asking -- and the disagreement only shows up
      // in game, never in the editor.
      bool rotateTo(std::uint32_t guid, double orientation);

      // Which spawn the UI has selected, so the renderer can outline it. 0 for none.
      //
      // Held here rather than in the panel because the render path needs it and already has the
      // cache; routing it through render params instead would mean threading a second pointer
      // through WorldRenderParams for one integer.
      void setSelected(std::uint32_t guid) { _selected_guid = guid; }
      std::uint32_t selected() const { return _selected_guid; }

      // Every loaded entry, in tile then load order. For listing them in the UI.
      std::vector<SpawnSceneEntry const*> allEntries() const;

      // Every entry the user has moved, in load order.
      std::vector<SpawnSceneEntry const*> dirtyEntries() const;

      std::size_t dirtyCount() const;

      // Forgets all edits, leaving the loaded scene alone. For "discard changes".
      void clearDirty();

      // Noggit-frame position of one loaded spawn, by guid. False when it is not loaded.
      //
      // Exists so a caller can aim at a spawn instead of computing where it ought to be and being
      // wrong -- which is a surprisingly effective way to conclude that nothing rendered.
      bool positionOf(std::uint32_t guid, glm::vec3& position) const;

      void clear();
      bool empty() const { return _tiles.empty(); }

      std::size_t tileCount() const { return _tiles.size(); }
      std::size_t instanceCount() const;
      SpawnSkipCounts skipped() const;

      // One line naming what loaded and what did not, for the log and the status bar.
      std::string summary() const;

      // Per-spawn detail: guid, kind, display id, resolved model, scale and the skins that were
      // resolved for it. For the dev bridge.
      //
      // Worth having as more than a debugging convenience: "the creature is black" has at least
      // four distinct causes -- no TextureVariation in the DBC row, a skin path that does not
      // exist in the archive, a model whose replaceable slots are typed differently, or the swap
      // failing at draw time -- and they are indistinguishable by looking at the viewport.
      std::string describe() const;

    private:
      Noggit::NoggitRenderContext _context;

      // Owned rather than passed in per call, so the two DBC chains stay cached across tiles and
      // across reloads. Both hops are linear scans and a miss costs a full scan plus a throw, so
      // rebuilding this per load would make reloading a tile set markedly more expensive than
      // loading it the first time.
      DisplayResolver _resolver;

      std::uint32_t _selected_guid = 0;

      // std::map, not unordered_map: ::TileIndex already provides operator< (TileIndex.hpp:14)
      // and no std::hash specialisation, and a tile count is at most a few hundred.
      std::map<::TileIndex, TileSpawnScene> _tiles;
  };
}

#endif
