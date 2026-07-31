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

  // What identifies one spawn. A guid on its own does NOT.
  //
  // `creature.guid` and `gameobject.guid` are independent primary keys in separate tables, each
  // counting up from 1, so their ranges overlap almost completely -- a tile holding creature 100
  // and gameobject 100 is ordinary, not a corner case. Every lookup here used to match on guid
  // alone and stop at the first hit, and setTile pushes all creatures before any gameobject
  // (SpawnSceneCache.cpp:117 then :152), so selecting gameobject 100 outlined both, moving it
  // moved the creature, and the changeset rewrote a row in the wrong table -- a spawn the user
  // never touched, changed silently.
  //
  // Kept as a struct rather than a packed 64-bit key so that the kind stays readable at every
  // call site; a caller that has to remember which half of a uint64 is the discriminator will
  // eventually get it wrong in exactly the way this type exists to prevent.
  struct SpawnRef
  {
    SpawnKind kind = SpawnKind::CREATURE;

    // 0 means "nothing selected". Neither table ever issues guid 0, so it cannot collide with a
    // real row.
    std::uint32_t guid = 0;

    bool valid() const { return guid != 0; }

    friend bool operator==(SpawnRef const& lhs, SpawnRef const& rhs)
    {
      return lhs.kind == rhs.kind && lhs.guid == rhs.guid;
    }

    friend bool operator!=(SpawnRef const& lhs, SpawnRef const& rhs) { return !(lhs == rhs); }
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

    // Set once the user moves it. Only dirty entries reach the changeset as EDITS.
    bool dirty = false;

    // True for a spawn the user placed, which no row of the database backs.
    //
    // > [!warning] `guid` is provisional for these, and must never reach SQL
    // > A created spawn needs an identity immediately -- to be selected, listed, dragged and
    // > deleted again -- but the database issues guids, and it will not issue this one until the
    // > changeset is applied. So the cache hands out a number above anything a real sequence has
    // > reached and this flag says it is not real. ChangesetBuilder::addNewCreature ignores the
    // > guid entirely and allocates from MAX(guid) at apply time.
    // >
    // > Two consequences that are easy to get wrong, and both silently destroy data:
    // >   - a created spawn is emitted as a CREATION even after it has been moved, never as an
    // >     edit, or the INSERT would carry the provisional guid and overwrite a real row;
    // >   - deleting one emits NO DELETE, because there is no row behind that number and the
    // >     sequence may since have reached it -- the DELETE would remove somebody else's spawn.
    bool is_new = false;

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

    // How the rest of the editor names this spawn. See SpawnRef on why the guid alone will not do.
    SpawnRef ref() const { return SpawnRef{kind, guid}; }
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

  // What came of a request to place a spawn.
  //
  // A reason rather than a bare false, because there are four distinct ways placing a spawn can
  // fail -- no template row, a template with displayId 0, a gameobject type that carries no model,
  // and a display id whose DBC chain resolves to nothing -- and they need four different answers
  // from the user. "Could not create it" sends them to look at the wrong thing.
  struct SpawnCreation
  {
    // Invalid (guid 0) when nothing was created.
    SpawnRef spawn;

    // Empty on success.
    std::string failure_reason;

    bool created() const { return spawn.valid(); }
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

      // Places a spawn the database has never seen, and returns how to name it.
      //
      // The caller supplies everything a row of `creature` holds plus the joined template info --
      // display id, type, name -- because resolving an entry id to a template is a database
      // question and this class holds no connection. MapView::createDatabaseSpawn does that half.
      //
      // The guid the caller passes is ignored: one is assigned here, from a range no real
      // sequence reaches, and SpawnSceneEntry::is_new marks it as provisional. See that flag --
      // getting this wrong overwrites a real row rather than erroring.
      //
      // Refuses a spawn whose model does not resolve, and says why. A spawn with no instance
      // cannot be picked, focused, dragged or seen: it would exist only as a line in a list and
      // as a row in a changeset the user had no way to check, which is worse than being told the
      // entry cannot be placed.
      //
      // > [!warning] An OpenGL context must be current, for the same reason setTile needs one.
      SpawnCreation addCreature(CreatureSpawn const& spawn);
      SpawnCreation addGameObject(GameObjectSpawn const& spawn);

      // Deletes a spawn, taking it out of the scene so it stops being drawn.
      //
      // What that means depends on where the spawn came from, and the two cases are genuinely
      // different rather than an implementation detail:
      //
      //   - A spawn the DATABASE issued is MOVED aside into a tombstone list. It becomes a DELETE
      //     in the changeset, and discardPending() can put it back exactly as it was rather than
      //     rebuilding it and re-queueing its asynchronous model load.
      //   - A spawn the USER placed is DESTROYED. No row backs it, so there is nothing to delete
      //     and nothing to restore -- discardPending() drops created spawns regardless. Keeping
      //     one as a tombstone made it invisible to every list at once: pendingCount() went to
      //     zero, the panel said "No changes." and disabled Save and Discard, and the spawn's
      //     model stayed loaded with no way left to reach it.
      //
      // Either way a spawn placed and deleted again before saving produces NO SQL: not an INSERT
      // followed by a DELETE, nothing at all.
      //
      // > [!warning] An OpenGL context must be current
      // > Destroying a created spawn releases its model reference, and the last reference to a
      // > Model destroys OpenGL vertex arrays from a destructor that throws when no context is
      // > bound -- which terminates the process. This did not need a context while every deletion
      // > was a move; it does now.
      bool remove(SpawnRef const& spawn);

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
      bool moveTo(SpawnRef const& spawn, glm::vec3 const& position);

      // Sets a spawn's facing, in server radians, and marks it dirty.
      //
      // For a gameobject this also rewrites rotation0..3. The core reads `orientation` but the
      // client renders the quaternion, so writing one without the other produces a spawn that
      // faces two different ways depending on who is asking -- and the disagreement only shows up
      // in game, never in the editor.
      bool rotateTo(SpawnRef const& spawn, double orientation);

      // Which spawn the UI has selected, so the renderer can outline it. Default-constructed
      // (guid 0) for none.
      //
      // Held here rather than in the panel because the render path needs it and already has the
      // cache; routing it through render params instead would mean threading a second pointer
      // through WorldRenderParams for one selection.
      void setSelected(SpawnRef const& spawn) { _selected = spawn; }
      SpawnRef selected() const { return _selected; }

      // Every loaded entry, in tile then load order. For listing them in the UI.
      std::vector<SpawnSceneEntry const*> allEntries() const;

      // Every entry the user has moved, in load order -- EDITS only.
      //
      // A created spawn is excluded even after it has been dragged, and that exclusion is
      // load-bearing rather than tidy. Such an entry is dirty and new at once; emitting it as an
      // edit would write its provisional guid into an INSERT keyed off @CGUID, which is a real
      // guid in the target database belonging to a spawn the user has never seen. It is emitted
      // once, as a creation, carrying whatever position it was dragged to.
      std::vector<SpawnSceneEntry const*> dirtyEntries() const;

      // Every spawn the user placed and has not saved yet, in creation order.
      std::vector<SpawnSceneEntry const*> newEntries() const;

      // Guids of the spawns the user deleted, for the DELETE statements.
      //
      // Only rows the database issued. A spawn that was created and then deleted in the same
      // session is simply gone -- remove() destroyed it -- so there is nothing to delete, and
      // naming its provisional guid would remove whatever the sequence has since put there.
      std::vector<SpawnRef> removedSpawns() const;

      std::size_t dirtyCount() const;

      // Everything unsaved: edits, creations and deletions. What the Save button is gated on and
      // what "loading discards N changes" has to count, since dirtyCount() alone silently loses
      // a session's worth of placements and deletions without warning.
      std::size_t pendingCount() const;

      // Forgets all edits, leaving the loaded scene alone. For "discard changes".
      //
      // Edits only -- it does not undo a creation or a deletion. discardPending() is what the UI
      // wants; this stays because the save path clears edit flags through clearPending(), and
      // because a caller that means "forget the moves" should not silently also resurrect deleted
      // spawns.
      void clearDirty();

      // Undoes every unsaved change: edit flags cleared, deleted spawns put back where they were,
      // created spawns dropped.
      //
      // The only undo this subsystem has. Database spawns do not participate in ActionManager --
      // nothing in src/noggit/database registers an action, and Ctrl+Z therefore does not reach
      // a spawn move, a placement or a deletion. Half-wiring one operation into ActionManager
      // would be worse than none: an undo stack that silently skips two of the three spawn
      // operations undoes the wrong thing when a user presses Ctrl+Z twice.
      //
      // > [!warning] An OpenGL context must be current: dropping a created spawn releases its
      // > model reference, and OpenGL::Scoped's destructor throws when none is bound.
      //
      // Returns how many created spawns were dropped, so the caller can say so.
      std::size_t discardPending();

      // Accepts every unsaved change, after the changeset carrying them has been written.
      //
      // Edit flags are cleared, deleted entries are destroyed for good, and created entries are
      // DROPPED from the scene rather than kept.
      //
      // Dropping them is the uncomfortable part and it is the only safe option. Their guids are
      // still provisional -- the file has been written, not necessarily applied, and even when it
      // has been the editor was not told which guids the server chose. Keeping them would leave
      // entries the user can drag whose next save either re-creates them (a duplicate spawn) or
      // writes their fictional guid into an UPDATE (a real spawn overwritten). Reloading the tile
      // is how they come back, with the guids the database issued.
      //
      // > [!warning] An OpenGL context must be current, for the same reason as discardPending.
      //
      // Returns how many created spawns were dropped.
      std::size_t clearPending();

      // Noggit-frame position of one loaded spawn. False when it is not loaded.
      //
      // Exists so a caller can aim at a spawn instead of computing where it ought to be and being
      // wrong -- which is a surprisingly effective way to conclude that nothing rendered.
      bool positionOf(SpawnRef const& spawn, glm::vec3& position) const;

      // Server-frame facing of one loaded spawn, in radians. False when it is not loaded.
      //
      // The counterpart of positionOf, and it exists for the same reason. rotateTo takes an
      // absolute orientation while a gizmo produces a per-frame delta, so the caller has to be
      // able to read the current value back. The alternative -- a caller accumulating its own
      // running total across a drag -- drifts away from the row the moment anything else touches
      // it, and reads the ModelInstance's dir.y instead, which is the display copy and carries
      // YAW_OFFSET_DEGREES baked in.
      //
      // Reads the stored row, not the instance, because the row is the authority: it is what
      // reaches the changeset.
      bool orientationOf(SpawnRef const& spawn, double& orientation) const;

      // Every loaded spawn carrying this guid, in tile then load order.
      //
      // For the one caller that genuinely starts from a bare number -- the dev bridge, whose
      // commands are typed as text. At most two results, since guid is a primary key within each
      // table, and two is the case that has to be reported rather than resolved: picking either
      // one would be the same coin flip that made a bare guid unusable as an identity in the
      // first place. Callers should treat size() != 1 as an error the user has to disambiguate.
      std::vector<SpawnRef> refsWithGuid(std::uint32_t guid) const;

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
      // Resolves one spawn into a drawable entry: display id, DBC chain, model path, instance,
      // skins. Shared by setTile and the two add* methods, because a spawn placed by the user has
      // to become exactly the same kind of entry as one read from the database -- if the two
      // built entries differently, a created spawn would render, pick or scale differently from
      // the identical row after a reload, and nothing would say so.
      //
      // `skipped` is bumped on the reason it failed, for the tile summary. `failure_reason` gets
      // the sentence, for a user who asked about this one spawn.
      bool buildEntry( CreatureSpawn const& creature
                     , SpawnSceneEntry& out
                     , SpawnSkipCounts& skipped
                     , std::string& failure_reason
                     );

      bool buildEntry( GameObjectSpawn const& object
                     , SpawnSceneEntry& out
                     , SpawnSkipCounts& skipped
                     , std::string& failure_reason
                     );

      // The next unused provisional guid. Skips anything already loaded, so a session that
      // somehow reached this range cannot be handed a number twice.
      std::uint32_t nextProvisionalGuid();

      // Puts a built entry into the tile its position falls in, and returns how to name it.
      SpawnCreation place(SpawnSceneEntry&& entry, WorldPosition const& position);

      Noggit::NoggitRenderContext _context;

      // Owned rather than passed in per call, so the two DBC chains stay cached across tiles and
      // across reloads. Both hops are linear scans and a miss costs a full scan plus a throw, so
      // rebuilding this per load would make reloading a tile set markedly more expensive than
      // loading it the first time.
      DisplayResolver _resolver;

      SpawnRef _selected;

      // std::map, not unordered_map: ::TileIndex already provides operator< (TileIndex.hpp:14)
      // and no std::hash specialisation, and a tile count is at most a few hundred.
      std::map<::TileIndex, TileSpawnScene> _tiles;

      // Database-issued entries taken out of the scene by remove(), with the ADT tile each came
      // from.
      //
      // Held rather than destroyed so discardPending() can put them back. That is the whole undo
      // story for a deletion, and it is why the deleted spawn keeps its model reference alive
      // meanwhile -- rebuilding one would re-queue an asynchronous load and lose the skins.
      //
      // Never holds a spawn with is_new set. One with no row behind it has no DELETE to emit and
      // nothing to be restored to, so remove() destroys it instead: an entry parked here that no
      // list reports is a spawn the user can neither see, save, nor discard.
      std::vector<std::pair<::TileIndex, SpawnSceneEntry>> _removed;

      // Next provisional guid to hand out, shared by both kinds.
      //
      // One counter for two key spaces on purpose. These numbers never reach SQL, so they need
      // only be unique WITHIN the cache, and a single counter makes that true without having to
      // reason about which sequence a SpawnRef came from. See PROVISIONAL_GUID_BASE in the .cpp
      // for why the range was chosen.
      std::uint32_t _next_provisional_guid;
  };
}

#endif
