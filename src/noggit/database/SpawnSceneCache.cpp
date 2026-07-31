// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/database/SpawnSceneCache.hpp>

#include <noggit/database/SpawnPlacement.hpp>
#include <noggit/database/TileCoordinates.hpp>
#include <noggit/Log.h>
#include <noggit/MapHeaders.h>

#include <Listfile.hpp>

#include <glm/vec3.hpp>

#include <sstream>
#include <utility>

using namespace Noggit::Database;

// The contract SpawnPlacement.hpp:20 requires of whatever glues it to a SceneObject.
//
// SpawnPlacement computes its own ZEROPOINT because the layer it lives in cannot include a
// Noggit header. Two independent copies of a magic number is exactly how a coordinate frame
// drifts, so the copy is verified here -- the one place that sees both. If this fires, the two
// disagree and every converted position is wrong by the difference; do not "fix" it by relaxing
// the comparison.
static_assert(SpawnPlacement::ZEROPOINT_F == ZEROPOINT
             , "SpawnPlacement::ZEROPOINT_F no longer matches Noggit's ZEROPOINT. Every database "
               "spawn position depends on the two being identical.");

namespace
{
  // Builds the instance for one already-resolved spawn.
  //
  // recalcExtents() is called even though the model is almost certainly still loading: it detects
  // that itself and defers by setting the dirty flag (ModelInstance.cpp:270-276), and
  // isInRenderDist/intersect call ensureExtents() before using them (ModelInstance.cpp:241). The
  // MDDF constructor sets that flag directly, but the plain (file_key, context) constructor this
  // uses does not, so skipping this call would leave extents at their opposite-infinity initial
  // values and the spawn would fail every distance test forever.
  std::unique_ptr<ModelInstance> makeInstance
    ( std::string const& model_path
    , NoggitPlacement const& placement
    , Noggit::NoggitRenderContext context
    )
  {
    auto instance
      (std::make_unique<ModelInstance>
        (BlizzardArchive::Listfile::FileKey(model_path), context));

    instance->pos = glm::vec3( static_cast<float>(placement.x)
                             , static_cast<float>(placement.y)
                             , static_cast<float>(placement.z)
                             );

    instance->dir = glm::vec3( static_cast<float>(placement.dir_x_degrees)
                             , static_cast<float>(placement.dir_y_degrees)
                             , static_cast<float>(placement.dir_z_degrees)
                             );

    instance->scale = static_cast<float>(placement.scale);

    // Explicit, not incidental. An unregistered instance with a plausible-looking uid is the one
    // way a database spawn could be mistaken for an ADT object by something walking the scene.
    instance->uid = 0;

    instance->recalcExtents();

    return instance;
  }

  // Both parameter and return type are spelled with an explicit scope on purpose. This file has
  // `using namespace Noggit::Database` above it and includes TileIndex.hpp, so there are two types
  // called TileIndex reachable by that one name -- and they are transposed with respect to each
  // other. Unqualified here it is ambiguous; qualified, the conversion this function exists to
  // perform is impossible to misread.
  // Turns resolved skin paths into held references, so the asynchronous BLP load survives long
  // enough to be uploaded. See the warning on SpawnSceneEntry::skin_textures.
  std::vector<std::pair<int, scoped_blp_texture_reference>> holdSkins
    ( std::vector<std::pair<int, std::string>> const& skins
    , Noggit::NoggitRenderContext context
    )
  {
    std::vector<std::pair<int, scoped_blp_texture_reference>> held;
    held.reserve(skins.size());

    for (auto const& skin : skins)
    {
      held.emplace_back(skin.first, scoped_blp_texture_reference(skin.second, context));
    }

    return held;
  }

  // Where provisional guids start.
  //
  // Above anything a real primary-key sequence reaches: `creature.guid` and `gameobject.guid`
  // count up from 1 and the largest world databases in circulation are six or seven digits, so
  // 0xF0000000 is a quarter of a billion clear of the top of any of them. Not that a collision
  // with a real guid would corrupt anything -- these numbers never reach SQL, and
  // SpawnSceneEntry::is_new is what keeps them out -- but a provisional guid that looked like a
  // plausible real one would make every log line and list entry ambiguous while debugging, which
  // is exactly when it matters that they are not.
  //
  // Not std::numeric_limits<std::uint32_t>::max() counting DOWN, which was the other obvious
  // choice: counting up keeps creation order and guid order agreeing, so the spawn list reads in
  // the order the user placed things.
  constexpr std::uint32_t PROVISIONAL_GUID_BASE = 0xF0000000u;

  ::TileIndex adtKeyFor(Noggit::Database::TileIndex const& db_tile)
  {
    Noggit::Database::AdtFileIndex const adt (Noggit::Database::toAdtFileIndex(db_tile));

    // Negative indices cannot occur for a tile derived from a real position, and ::TileIndex
    // takes std::size_t, so a negative would wrap to an enormous index and quietly never match
    // a loaded tile. Clamped rather than asserted: a malformed row must not take the editor down.
    return ::TileIndex( static_cast<std::size_t>(adt.x < 0 ? 0 : adt.x)
                      , static_cast<std::size_t>(adt.z < 0 ? 0 : adt.z)
                      );
  }
}

SpawnSceneCache::SpawnSceneCache(Noggit::NoggitRenderContext context)
  : _context(context)
  , _next_provisional_guid(PROVISIONAL_GUID_BASE)
{
}

bool SpawnSceneCache::buildEntry
  ( CreatureSpawn const& creature
  , SpawnSceneEntry& out
  , SpawnSkipCounts& skipped
  , std::string& failure_reason
  )
{
  if (!SpawnDisplay::isRenderable(creature))
  {
    // For a creature the only way isRenderable fails is an unresolved display id -- there is
    // no per-type exclusion as there is for gameobjects.
    ++skipped.no_display_id;
    failure_reason = "creature_template entry " + std::to_string(creature.id)
                   + " names no model: creature.modelid is 0 and every modelid1..4 on the"
                     " template is 0 too, so there is nothing to draw.";
    return false;
  }

  std::uint32_t const display_id = creature.template_info.display_id;
  ResolvedModel const resolved (_resolver.creatureModel(display_id));

  if (!resolved.resolved)
  {
    ++skipped.unresolved_model;
    LogDebug << "Creature guid " << creature.guid << " display " << display_id
             << " has no usable model: " << resolved.failure_reason << std::endl;

    failure_reason = "display id " + std::to_string(display_id) + " resolves to no model: "
                   + resolved.failure_reason;
    return false;
  }

  out.instance = makeInstance(resolved.model_path
                             , SpawnPlacement::placementFor(creature, resolved.scale)
                             , _context);
  out.kind = SpawnKind::CREATURE;
  out.guid = creature.guid;
  out.display_id = display_id;
  out.creature = creature;
  out.skins = resolved.skins;
  out.skin_textures = holdSkins(resolved.skins, _context);

  return true;
}

bool SpawnSceneCache::buildEntry
  ( GameObjectSpawn const& object
  , SpawnSceneEntry& out
  , SpawnSkipCounts& skipped
  , std::string& failure_reason
  )
{
  // Checked before the display id so the two reasons stay distinguishable: a trap with a
  // perfectly good displayId is not the same problem as a chest with none.
  if (!SpawnDisplay::typeHasRenderableModel(object.template_info.type))
  {
    ++skipped.non_renderable_type;
    failure_reason = "GAMEOBJECT_TYPE " + std::to_string(object.template_info.type)
                   + " carries no model the editor can draw whatever its displayId says --"
                     " traps, transports, cameras and ritual objects are invisible by nature.";
    return false;
  }

  if (!SpawnDisplay::isRenderable(object))
  {
    ++skipped.no_display_id;
    failure_reason = "gameobject_template entry " + std::to_string(object.id)
                   + " has displayId 0, so there is nothing to draw.";
    return false;
  }

  std::uint32_t const display_id = object.template_info.display_id;
  ResolvedModel const resolved (_resolver.gameObjectModel(display_id));

  if (!resolved.resolved)
  {
    ++skipped.unresolved_model;
    LogDebug << "Gameobject guid " << object.guid << " display " << display_id
             << " has no usable model: " << resolved.failure_reason << std::endl;

    failure_reason = "display id " + std::to_string(display_id) + " resolves to no model: "
                   + resolved.failure_reason;
    return false;
  }

  out.instance = makeInstance(resolved.model_path
                             , SpawnPlacement::placementFor(object, resolved.scale)
                             , _context);
  out.kind = SpawnKind::GAMEOBJECT;
  out.guid = object.guid;
  out.display_id = display_id;
  out.gameobject = object;
  out.skins = resolved.skins;
  out.skin_textures = holdSkins(resolved.skins, _context);

  return true;
}

void SpawnSceneCache::setTile(TileSpawns const& spawns)
{
  TileSpawnScene scene;
  scene.entries.reserve(spawns.creatures.size() + spawns.gameobjects.size());

  // Discarded: the per-spawn sentence is for a user who asked about one spawn. Loading a tile
  // produces a count by reason instead, which is what makes an empty overlay explainable without
  // printing a line per skipped row.
  std::string reason;

  for (auto const& creature : spawns.creatures)
  {
    SpawnSceneEntry entry;

    if (buildEntry(creature, entry, scene.skipped, reason))
    {
      scene.entries.push_back(std::move(entry));
    }
  }

  for (auto const& object : spawns.gameobjects)
  {
    SpawnSceneEntry entry;

    if (buildEntry(object, entry, scene.skipped, reason))
    {
      scene.entries.push_back(std::move(entry));
    }
  }

  // insert_or_assign, not insert: reloading a tile has to replace what was there, and insert()
  // would silently keep the stale scene so an edited row appeared not to have changed.
  _tiles.insert_or_assign(adtKeyFor(spawns.tile), std::move(scene));
}

std::uint32_t SpawnSceneCache::nextProvisionalGuid()
{
  // Linear over everything loaded, which is at most a few thousand entries and runs once per
  // placement -- not per frame. The scan exists because the counter is not the only source of
  // guids in the cache: setTile fills it with whatever the database issued, and a database that
  // had somehow reached this range would otherwise be handed a number already in use, which
  // makes two spawns indistinguishable to every SpawnRef lookup in this class.
  for (;;)
  {
    // Wrapped back into the provisional range rather than allowed to reach 0 and count up from
    // there. It takes 268 million placements in one session to get here and none of them would
    // survive the memory cost, but the failure it would cause -- provisional guids colliding
    // with the low numbers real spawns actually use -- is silent, so it costs one comparison to
    // make impossible rather than merely unlikely.
    if (_next_provisional_guid < PROVISIONAL_GUID_BASE)
    {
      _next_provisional_guid = PROVISIONAL_GUID_BASE;
    }

    std::uint32_t const candidate (_next_provisional_guid++);

    bool taken = false;

    for (auto const& tile : _tiles)
    {
      for (auto const& entry : tile.second.entries)
      {
        if (entry.guid == candidate)
        {
          taken = true;
          break;
        }
      }

      if (taken)
      {
        break;
      }
    }

    // Deleted-but-restorable entries count as taken: discardPending() puts them back, and a
    // number handed out twice would make the restored one unreachable.
    for (auto const& removed : _removed)
    {
      if (removed.second.guid == candidate)
      {
        taken = true;
        break;
      }
    }

    if (!taken)
    {
      return candidate;
    }
  }
}

SpawnCreation SpawnSceneCache::place(SpawnSceneEntry&& entry, WorldPosition const& position)
{
  // The tile is derived from the position, never taken from the caller. Which tile a spawn
  // belongs to is exactly the question the two transposed TileIndex types make easy to answer
  // wrongly, and tileForPosition plus adtKeyFor is the one path through it that is tested.
  ::TileIndex const adt_tile (adtKeyFor(TileCoordinates::tileForPosition(position)));

  SpawnRef const ref (entry.ref());

  // operator[] rather than find: placing into a tile nothing was loaded for is normal -- the
  // user can put a creature anywhere the camera reaches, including an ADT whose spawns were
  // never queried -- and it has to become a scene the renderer will walk.
  _tiles[adt_tile].entries.push_back(std::move(entry));

  return SpawnCreation{ref, std::string()};
}

SpawnCreation SpawnSceneCache::addCreature(CreatureSpawn const& spawn)
{
  CreatureSpawn placed (spawn);
  placed.guid = nextProvisionalGuid();

  SpawnSceneEntry entry;
  SpawnSkipCounts ignored;
  std::string reason;

  if (!buildEntry(placed, entry, ignored, reason))
  {
    // Refused rather than created invisibly. An entry with no instance cannot be picked,
    // focused, dragged or seen, so the user would have placed something they can only find out
    // about by reading the changeset -- and could not move if it landed in the wrong place.
    return SpawnCreation{SpawnRef{}, reason};
  }

  entry.is_new = true;

  return place(std::move(entry), placed.position);
}

SpawnCreation SpawnSceneCache::addGameObject(GameObjectSpawn const& spawn)
{
  GameObjectSpawn placed (spawn);
  placed.guid = nextProvisionalGuid();

  SpawnSceneEntry entry;
  SpawnSkipCounts ignored;
  std::string reason;

  if (!buildEntry(placed, entry, ignored, reason))
  {
    return SpawnCreation{SpawnRef{}, reason};
  }

  entry.is_new = true;

  return place(std::move(entry), placed.position);
}

bool SpawnSceneCache::remove(SpawnRef const& spawn)
{
  if (!spawn.valid())
  {
    return false;
  }

  for (auto& tile : _tiles)
  {
    auto& entries (tile.second.entries);

    for (std::size_t i = 0; i < entries.size(); ++i)
    {
      // Kind as well as guid, for the same reason moveTo matches on both. Deleting by a bare
      // guid would delete whichever of the two tables' rows the scan reached first.
      if (entries[i].ref() != spawn)
      {
        continue;
      }

      // Two different operations wearing one name, and the difference is not cosmetic.
      //
      // A spawn the DATABASE issued is moved aside into _removed. It has a row behind it, that
      // row has to become a DELETE, and discardPending() has to be able to put the spawn back --
      // which is why it keeps its model reference rather than being rebuilt later and re-queueing
      // an asynchronous load.
      //
      // A spawn the USER placed has none of that. There is no row, so there is no DELETE (see
      // removedSpawns), and there is nothing to restore it to: discardPending() drops created
      // spawns anyway. Parking one in _removed therefore produced an entry that no list reported
      // -- dirtyEntries() and newEntries() walk _tiles, which no longer held it, and
      // removedSpawns() skipped it for being new -- so pendingCount() said 0, the panel said "No
      // changes.", and both Save and Discard went grey while the spawn's model stayed resident
      // and unreachable for the rest of the session. Created and then deleted means it never
      // happened; the honest representation of that is for it to stop existing.
      if (!entries[i].is_new)
      {
        _removed.emplace_back(tile.first, std::move(entries[i]));
      }

      // > [!warning] This is where a model reference can be released
      // > Erasing a created entry destroys its ModelInstance, and the last reference to a Model
      // > destroys OpenGL vertex arrays from a destructor that THROWS when no context is bound
      // > (context.inl:47) -- a terminate, not a catchable error. For a database-issued spawn the
      // > entry was moved out first and this erases a hollow shell, so nothing is released; for a
      // > created one it is. Callers must bind a context, as MapView::deleteDatabaseSpawn does.
      entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(i));

      // A deleted spawn must not stay selected: the gizmo would keep a target that is no longer
      // drawn, and the panel would show a facing for a row that is going away.
      if (_selected == spawn)
      {
        _selected = SpawnRef{};
      }

      return true;
    }
  }

  return false;
}

std::vector<SpawnSceneEntry const*> SpawnSceneCache::newEntries() const
{
  std::vector<SpawnSceneEntry const*> created;

  for (auto const& tile : _tiles)
  {
    for (auto const& entry : tile.second.entries)
    {
      if (entry.is_new)
      {
        created.push_back(&entry);
      }
    }
  }

  return created;
}

std::vector<SpawnRef> SpawnSceneCache::removedSpawns() const
{
  std::vector<SpawnRef> refs;

  for (auto const& removed : _removed)
  {
    // Belt to remove()'s braces, and kept deliberately although remove() no longer puts a created
    // spawn in _removed at all. A provisional guid may since have been issued by the real sequence
    // to a spawn nobody in this session has ever seen, so a DELETE naming one would remove
    // somebody else's data -- silently, and with no way to tell afterwards. That is worth one
    // comparison to make impossible rather than merely true of the current call graph.
    if (!removed.second.is_new)
    {
      refs.push_back(removed.second.ref());
    }
  }

  return refs;
}

std::size_t SpawnSceneCache::pendingCount() const
{
  return dirtyEntries().size() + newEntries().size() + removedSpawns().size();
}

std::size_t SpawnSceneCache::discardPending()
{
  // Deleted spawns go back into the tile they came from, so the outcome is what the user sees
  // before the deletion -- not a reload.
  for (auto& removed : _removed)
  {
    _tiles[removed.first].entries.push_back(std::move(removed.second));
  }

  _removed.clear();

  // AFTER the restore, not before. A spawn that was moved and then deleted comes back carrying
  // the dirty flag it had when it was taken out; clearing first would leave that flag set on the
  // restored entry, and the next save would emit an edit the user had just discarded.
  clearDirty();

  std::size_t dropped = 0;

  for (auto& tile : _tiles)
  {
    auto& entries (tile.second.entries);

    for (std::size_t i = entries.size(); i > 0; --i)
    {
      if (!entries[i - 1].is_new)
      {
        continue;
      }

      // This is where a model reference is actually released, and why the header demands an
      // OpenGL context: the last reference to a Model destroys its ModelRender and its vertex
      // arrays, and OpenGL::Scoped's destructor throws when none is bound -- from a destructor,
      // which terminates rather than raising.
      entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(i - 1));
      ++dropped;
    }
  }

  _selected = SpawnRef{};

  return dropped;
}

std::size_t SpawnSceneCache::clearPending()
{
  clearDirty();

  // Destroyed for good: the changeset carrying their DELETE has been written.
  _removed.clear();

  std::size_t dropped = 0;

  for (auto& tile : _tiles)
  {
    auto& entries (tile.second.entries);

    for (std::size_t i = entries.size(); i > 0; --i)
    {
      if (!entries[i - 1].is_new)
      {
        continue;
      }

      entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(i - 1));
      ++dropped;
    }
  }

  if (dropped > 0)
  {
    // The selection may have been one of them, and a stale ref would leave the gizmo attached to
    // nothing. Cleared unconditionally rather than compared, since the created entries are gone
    // and there is nothing left to compare against.
    _selected = SpawnRef{};
  }

  return dropped;
}

TileSpawnScene const* SpawnSceneCache::tile(::TileIndex const& adt_tile) const
{
  auto const found (_tiles.find(adt_tile));
  return found == _tiles.end() ? nullptr : &found->second;
}

bool SpawnSceneCache::moveTo(SpawnRef const& spawn, glm::vec3 const& position)
{
  for (auto& tile : _tiles)
  {
    for (auto& entry : tile.second.entries)
    {
      // Kind as well as guid. Matching on guid alone stopped at the creature that shares the
      // number, so moving a gameobject silently rewrote creature.position instead -- see SpawnRef.
      if (entry.ref() != spawn || !entry.instance)
      {
        continue;
      }

      entry.instance->pos = position;
      entry.instance->recalcExtents();

      // Back through the tested inverse rather than by hand. serverPositionFor is the exact
      // counterpart of positionFor, so a spawn moved and not moved again re-emits the coordinates
      // it was read with -- which is what keeps an untouched row out of the diff.
      NoggitPlacement placement;
      placement.x = position.x;
      placement.y = position.y;
      placement.z = position.z;

      WorldPosition const world (SpawnPlacement::serverPositionFor(placement));

      if (entry.kind == SpawnKind::CREATURE)
      {
        entry.creature.position = world;
      }
      else
      {
        entry.gameobject.position = world;
      }

      entry.dirty = true;

      return true;
    }
  }

  return false;
}

bool SpawnSceneCache::rotateTo(SpawnRef const& spawn, double orientation)
{
  double const normalised (TileCoordinates::normaliseOrientation(orientation));

  for (auto& tile : _tiles)
  {
    for (auto& entry : tile.second.entries)
    {
      // Kind as well as guid, for the same reason as moveTo: a bare guid can name a row in
      // either table.
      if (entry.ref() != spawn || !entry.instance)
      {
        continue;
      }

      if (entry.kind == SpawnKind::CREATURE)
      {
        entry.creature.orientation = normalised;
      }
      else
      {
        entry.gameobject.orientation = normalised;

        // Both, always. The core reads `orientation`; the client renders the quaternion. A spawn
        // with only one updated faces differently in the editor and in game.
        entry.gameobject.rotation = TileCoordinates::quaternionForOrientation(normalised);
      }

      // Through the same converter the load path uses, so the viewport shows exactly what the
      // emitted row will mean -- including YAW_OFFSET_DEGREES, which is not something a call site
      // should be applying by hand.
      NoggitPlacement const placement
        ( entry.kind == SpawnKind::CREATURE
            ? SpawnPlacement::placementFor(entry.creature)
            : SpawnPlacement::placementFor(entry.gameobject)
        );

      entry.instance->dir.y = static_cast<float>(placement.dir_y_degrees);
      entry.instance->recalcExtents();

      entry.dirty = true;

      return true;
    }
  }

  return false;
}

std::vector<SpawnSceneEntry const*> SpawnSceneCache::allEntries() const
{
  std::vector<SpawnSceneEntry const*> all;

  for (auto const& tile : _tiles)
  {
    for (auto const& entry : tile.second.entries)
    {
      all.push_back(&entry);
    }
  }

  return all;
}

std::vector<SpawnSceneEntry const*> SpawnSceneCache::dirtyEntries() const
{
  std::vector<SpawnSceneEntry const*> dirty;

  for (auto const& tile : _tiles)
  {
    for (auto const& entry : tile.second.entries)
    {
      // dirty AND NOT new. A created spawn that has since been dragged is both, and emitting it
      // here as well as in newEntries() would put it in the changeset twice: once as an INSERT
      // keyed off @CGUID_NEW, and once as a DELETE-then-INSERT keyed off @CGUID -- the second of
      // which names its provisional guid, a number that in the target database belongs to some
      // other spawn entirely. See SpawnSceneEntry::is_new.
      if (entry.dirty && !entry.is_new)
      {
        dirty.push_back(&entry);
      }
    }
  }

  return dirty;
}

std::size_t SpawnSceneCache::dirtyCount() const
{
  return dirtyEntries().size();
}

void SpawnSceneCache::clearDirty()
{
  for (auto& tile : _tiles)
  {
    for (auto& entry : tile.second.entries)
    {
      entry.dirty = false;
    }
  }
}

bool SpawnSceneCache::positionOf(SpawnRef const& spawn, glm::vec3& position) const
{
  for (auto const& tile : _tiles)
  {
    for (auto const& entry : tile.second.entries)
    {
      if (entry.ref() == spawn && entry.instance)
      {
        position = entry.instance->pos;
        return true;
      }
    }
  }

  return false;
}

bool SpawnSceneCache::orientationOf(SpawnRef const& spawn, double& orientation) const
{
  for (auto const& tile : _tiles)
  {
    for (auto const& entry : tile.second.entries)
    {
      // Kind as well as guid, for the same reason moveTo and rotateTo match on both: a bare guid
      // names a row in either table.
      if (entry.ref() != spawn)
      {
        continue;
      }

      orientation = entry.kind == SpawnKind::CREATURE
                      ? entry.creature.orientation
                      : entry.gameobject.orientation;

      return true;
    }
  }

  return false;
}

std::vector<SpawnRef> SpawnSceneCache::refsWithGuid(std::uint32_t guid) const
{
  std::vector<SpawnRef> matches;

  if (guid == 0)
  {
    return matches;
  }

  for (auto const& tile : _tiles)
  {
    for (auto const& entry : tile.second.entries)
    {
      if (entry.guid == guid)
      {
        matches.push_back(entry.ref());
      }
    }
  }

  return matches;
}

void SpawnSceneCache::clear()
{
  _tiles.clear();

  // Deleted-but-restorable entries go too. Reloading already warns that unsaved changes are
  // discarded, and keeping tombstones across a reload would emit a DELETE for a spawn the user
  // can no longer see and has had no chance to reconsider.
  _removed.clear();

  // The counter is deliberately NOT reset. Provisional guids stay unique for the lifetime of the
  // cache, so a ref held across a reload -- by the panel, the gizmo or a log line -- cannot come
  // to name a different spawn than it did when it was taken.
}

std::size_t SpawnSceneCache::instanceCount() const
{
  std::size_t count = 0;

  for (auto const& tile : _tiles)
  {
    count += tile.second.entries.size();
  }

  return count;
}

SpawnSkipCounts SpawnSceneCache::skipped() const
{
  SpawnSkipCounts totals;

  for (auto const& tile : _tiles)
  {
    totals.no_display_id += tile.second.skipped.no_display_id;
    totals.non_renderable_type += tile.second.skipped.non_renderable_type;
    totals.unresolved_model += tile.second.skipped.unresolved_model;
  }

  return totals;
}

std::string SpawnSceneCache::describe() const
{
  std::ostringstream out;

  for (auto const& tile : _tiles)
  {
    for (auto const& entry : tile.second.entries)
    {
      out << " | guid=" << entry.guid
          << (entry.is_new ? " (provisional, not in the database)" : "")
          << " kind=" << (entry.kind == SpawnKind::CREATURE ? "creature" : "gameobject")
          << " display=" << entry.display_id;

      if (entry.instance)
      {
        out << " model=" << entry.instance->model->file_key().stringRepr()
            << " scale=" << entry.instance->scale
            << " loaded=" << (entry.instance->model->finishedLoading() ? "yes" : "no")
            << " failed=" << (entry.instance->model->loading_failed() ? "yes" : "no");

        // The actual type values, not just how many are non-zero. The swap at draw time matches
        // a skin's type against these, so a mismatch here is indistinguishable from "no skins
        // resolved" by looking at the model -- and produces exactly the same black silhouette.
        out << " slot_types=[";

        for (std::size_t i = 0; i < entry.instance->model->_replaceable_texture_types.size(); ++i)
        {
          out << (i ? "," : "") << entry.instance->model->_replaceable_texture_types[i];
        }

        out << "] textures=" << entry.instance->model->_textures.size();
      }

      out << " skins=" << entry.skins.size();

      for (auto const& skin : entry.skins)
      {
        out << " [type" << skin.first << '=' << skin.second << ']';
      }
    }
  }

  std::string const text (out.str());

  return text.empty() ? std::string(" (nothing loaded)") : text;
}

std::string SpawnSceneCache::summary() const
{
  SpawnSkipCounts const totals (skipped());

  std::ostringstream out;
  out << instanceCount() << " spawn(s) across " << _tiles.size() << " tile(s)";

  if (totals.total() > 0)
  {
    out << "; " << totals.total() << " not drawn ("
        << totals.no_display_id << " no display id, "
        << totals.non_renderable_type << " non-renderable type, "
        << totals.unresolved_model << " unresolved model)";
  }

  return out.str();
}
