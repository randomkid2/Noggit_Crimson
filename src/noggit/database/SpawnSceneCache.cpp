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
{
}

void SpawnSceneCache::setTile(TileSpawns const& spawns)
{
  TileSpawnScene scene;
  scene.entries.reserve(spawns.creatures.size() + spawns.gameobjects.size());

  for (auto const& creature : spawns.creatures)
  {
    if (!SpawnDisplay::isRenderable(creature))
    {
      // For a creature the only way isRenderable fails is an unresolved display id -- there is
      // no per-type exclusion as there is for gameobjects.
      ++scene.skipped.no_display_id;
      continue;
    }

    std::uint32_t const display_id = creature.template_info.display_id;
    ResolvedModel const resolved (_resolver.creatureModel(display_id));

    if (!resolved.resolved)
    {
      ++scene.skipped.unresolved_model;
      LogDebug << "Creature guid " << creature.guid << " display " << display_id
               << " has no usable model: " << resolved.failure_reason << std::endl;
      continue;
    }

    SpawnSceneEntry entry;
    entry.instance = makeInstance(resolved.model_path
                                 , SpawnPlacement::placementFor(creature, resolved.scale)
                                 , _context);
    entry.kind = SpawnKind::CREATURE;
    entry.guid = creature.guid;
    entry.display_id = display_id;
    entry.creature = creature;
    entry.skins = resolved.skins;
    entry.skin_textures = holdSkins(resolved.skins, _context);

    scene.entries.push_back(std::move(entry));
  }

  for (auto const& object : spawns.gameobjects)
  {
    // Checked before the display id so the two reasons stay distinguishable: a trap with a
    // perfectly good displayId is not the same problem as a chest with none.
    if (!SpawnDisplay::typeHasRenderableModel(object.template_info.type))
    {
      ++scene.skipped.non_renderable_type;
      continue;
    }

    if (!SpawnDisplay::isRenderable(object))
    {
      ++scene.skipped.no_display_id;
      continue;
    }

    std::uint32_t const display_id = object.template_info.display_id;
    ResolvedModel const resolved (_resolver.gameObjectModel(display_id));

    if (!resolved.resolved)
    {
      ++scene.skipped.unresolved_model;
      LogDebug << "Gameobject guid " << object.guid << " display " << display_id
               << " has no usable model: " << resolved.failure_reason << std::endl;
      continue;
    }

    SpawnSceneEntry entry;
    entry.instance = makeInstance(resolved.model_path
                                 , SpawnPlacement::placementFor(object, resolved.scale)
                                 , _context);
    entry.kind = SpawnKind::GAMEOBJECT;
    entry.guid = object.guid;
    entry.display_id = display_id;
    entry.gameobject = object;
    entry.skins = resolved.skins;
    entry.skin_textures = holdSkins(resolved.skins, _context);

    scene.entries.push_back(std::move(entry));
  }

  // insert_or_assign, not insert: reloading a tile has to replace what was there, and insert()
  // would silently keep the stale scene so an edited row appeared not to have changed.
  _tiles.insert_or_assign(adtKeyFor(spawns.tile), std::move(scene));
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
      if (entry.dirty)
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
