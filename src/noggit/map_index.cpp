// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <math/coordinates.hpp>
#include <noggit/AsyncLoader.h>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/Misc.h>
#include <noggit/World.h>
#include <noggit/ActionManager.hpp>
#include <noggit/Action.hpp>
#include <noggit/project/CurrentProject.hpp>
#ifdef USE_MYSQL_UID_STORAGE
  #include <mysql/mysql.h>
#endif
#include <noggit/map_index.hpp>
#include <noggit/SafeFileWrite.hpp>
#include <noggit/terrain/TerrainMaskStore.hpp>
#include <noggit/uid_storage.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <ClientFile.hpp>
#include <Exception.hpp>

#include <QtCore/QSettings>
#include <QByteArray>
#include <QTextStream>
#include <QRegExp>
#include <QFile>

#include <filesystem>
#include <forward_list>
#include <sstream>
#include <string>

MapIndex::TileRange<false> MapIndex::loaded_tiles()
{
  return tiles<false>
    ([](TileIndex const&, MapTile* tile) { return !!tile && tile->finishedLoading(); });
}

MapIndex::TileRange<true> MapIndex::tiles_in_range(glm::vec3 const& pos, float radius)
{
  return tiles<true>
    ([this, pos, radius](TileIndex const& index, MapTile*)
      {
        return hasTile(index) && misc::getShortestDist
        (pos.x, pos.z, index.x * TILESIZE, index.z * TILESIZE, TILESIZE) <= radius;
      }
    );
}

MapIndex::TileRange<true> MapIndex::tiles_in_rect(glm::vec3 const& pos, float radius)
{
  glm::vec2 l_chunk{ pos.x - radius, pos.z - radius };
  glm::vec2 r_chunk{ pos.x + radius, pos.z + radius };

  return tiles<true>
    ([this, pos, radius, l_chunk, r_chunk](TileIndex const& index, MapTile*)
      {
        if (!hasTile(index) || radius == 0.f)
          return false;

        glm::vec2 l_tile{ index.x * TILESIZE, index.z * TILESIZE };
        glm::vec2 r_tile{ index.x * TILESIZE + TILESIZE, index.z * TILESIZE + TILESIZE };

        return ((l_chunk.x  <  r_tile.x) && (r_chunk.x >= l_tile.x) && (l_chunk.y  <  r_tile.y) && (r_chunk.y >= l_tile.y));
      }
    );
}

MapIndex::MapIndex (const std::string &pBasename, int map_id, World* world,
                    Noggit::NoggitRenderContext context, bool create_empty)
  : basename(pBasename)
  , _map_id (map_id)
  , _last_unload_time((clock() / CLOCKS_PER_SEC)) // to not try to unload right away
  , mBigAlpha(false)
  , mHasAGlobalWMO(false)
  , changed(false)
  , _sort_models_by_size_class(false)
  , highestGUID(0)
  , _world (world)
  , _context(context)
{

  QSettings settings;
  _unload_interval = settings.value("unload_interval", 30).toInt();
  _unload_dist = settings.value("unload_dist", 5).toInt();
  _loading_radius = settings.value("loading_radius", 2).toInt();

  if (create_empty)
  {

    mHasAGlobalWMO = false;
    mBigAlpha = true;
    _sort_models_by_size_class = true;
    changed = false;

    for (int j = 0; j < 64; ++j)
    {
      for (int i = 0; i < 64; ++i)
      {
        mTiles[j][i].tile = nullptr;
        mTiles[j][i].onDisc = false;
        mTiles[j][i].flags = 0;
      }
    }

    return;
  }

  std::stringstream filename;
  filename << "World\\Maps\\" << basename << "\\" << basename << ".wdt";

  BlizzardArchive::ClientFile theFile(filename.str(), Noggit::Application::NoggitApplication::instance()->clientData());

  uint32_t fourcc;
  uint32_t size;

  // - MVER ----------------------------------------------

  uint32_t version;

  theFile.read(&fourcc, 4);
  theFile.read(&size, 4);
  theFile.read(&version, 4);

  //! \todo find the correct version of WDT files.
  assert(fourcc == 'MVER' && version == 18);

  // - MHDR ----------------------------------------------

  theFile.read(&fourcc, 4);
  theFile.read(&size, 4);

  assert(fourcc == 'MPHD');

  theFile.read(&mphd, sizeof(MPHD));

  mHasAGlobalWMO = mphd.flags & FLAG_GLOBAL_OBJECT;
  mBigAlpha = mphd.flags & FLAG_BIG_ALPHA;
  _sort_models_by_size_class = mphd.flags & FLAG_DOODADS_SORT;

  if (!(mphd.flags & FLAG_SHADING))
  {
    mphd.flags |= FLAG_SHADING;
    changed = true;
  }

  // - MAIN ----------------------------------------------

  theFile.read(&fourcc, 4);
  theFile.seekRelative(4);

  assert(fourcc == 'MAIN');

  /// this is the theory. Sadly, we are also compiling on 64 bit machines with size_t being 8 byte, not 4. Therefore, we can't do the same thing, Blizzard does in its 32bit executable.
  //theFile.read( &(mTiles[0][0]), sizeof( 8 * 64 * 64 ) );

  // We could skip for WMO only maps
  for (int j = 0; j < 64; ++j)
  {
    for (int i = 0; i < 64; ++i)
    {
      theFile.read(&mTiles[j][i].flags, 4);
      theFile.seekRelative(4);

      mTiles[j][i].tile = nullptr;

      if (!(mTiles[j][i].flags & 1))
        continue;

      std::stringstream adt_filename;
      adt_filename << "World\\Maps\\" << basename << "\\" << basename << "_" << i << "_" << j << ".adt";

      mTiles[j][i].onDisc = Noggit::Application::NoggitApplication::instance()->clientData()->existsOnDisk(adt_filename.str());

			if (mTiles[j][i].onDisc)
			{
				mTiles[j][i].flags |= 1;
				changed = true;
			}
		}
	}

  if (!theFile.isEof() && mHasAGlobalWMO)
  {
    //! \note We actually don't load WMO only worlds, so we just stop reading here, k?
    //! \bug MODF reads wrong. The assertion fails every time. Somehow, it keeps being MWMO. Or are there two blocks?
    //! \nofuckingbug  on eof read returns just without doing sth to the var and some wdts have a MWMO without having a MODF so only checking for eof above is not enough

    // mHasAGlobalWMO = false;

    // - MWMO ----------------------------------------------

    theFile.read(&fourcc, 4);
    theFile.read(&size, 4);

    assert(fourcc == 'MWMO');

    globalWMOName = std::string(theFile.getPointer(), size);
    theFile.seekRelative(size);

    // - MODF ----------------------------------------------

    theFile.read(&fourcc, 4);
    theFile.read(&size, 4);

    assert(fourcc == 'MODF');

    theFile.read(&wmoEntry, sizeof(ENTRY_MODF));
    math::to_client(wmoEntry.pos);
  }

  // -----------------------------------------------------

  theFile.close();

  loadMinimapMD5translate();
}

// WHOLE-MAP FAILURE POLICY: keep going, and report every failure at the end.
//
// Saving a map writes up to 4096 independent files. The alternative -- stop at the first failure --
// was rejected for three measured reasons.
//
// First, stopping cannot save anything that continuing loses. Each tile is its own file and its own
// atomic replace; tile 41 is not made any safer by refusing to attempt it after tile 40 failed. If
// the cause is local (one read-only ADT, one file the client has open, one path the antivirus is
// scanning) then stopping punishes up to 4095 healthy tiles for one sick one, and the user has no
// way to find out which of the rest would have worked.
//
// Second, continuing is now cheap and harmless. Every attempt writes a sibling temp file and only
// renames it into place once it is complete, so a tile that fails costs one create and one delete
// and leaves its destination byte-for-byte as it was. Before this change continuing WOULD have been
// the wrong answer, because every attempt truncated a real ADT first.
//
// Third, and this is the part that actually loses work: `changed` is now cleared only for tiles
// that reached the disk. MapIndex::unloadTiles refuses to release a tile whose `changed` flag is
// set, so a tile that failed to save stays in memory and stays marked on the minimap, and the user
// can fix the cause and save again. The old code cleared `changed` unconditionally after a save
// call that could not report failure -- that combination is what turned a failed write into
// permanently lost edits, and it is the same combination in both loops below.
bool MapIndex::saveall (World* world)
{
  world->wait_for_all_tile_updates();

  saveMaxUID();

  bool all_saved (true);

  for (MapTile* tile : loaded_tiles())
  {
    world->horizon.update_horizon_tile(tile);

    if (tile->saveTile(world))
    {
      tile->changed = false;
    }
    else
    {
      all_saved = false;
    }
  }

  return all_saved;
}

bool MapIndex::save()
{
  std::stringstream filename;
  filename << "World\\Maps\\" << basename << "\\" << basename << ".wdt";

  //Log << "Saving WDT \"" << filename << "\"." << std::endl;

  util::sExtendableArray wdtFile;
  int curPos = 0;

  // MVER
  //  {
  wdtFile.Extend(8 + 0x4);
  SetChunkHeader(wdtFile, curPos, 'MVER', 4);

  // MVER data
  *(wdtFile.GetPointer<int>(8)) = 18;

  curPos += 8 + 0x4;
  //  }

  // MPHD
  //  {
  wdtFile.Extend(8);
  SetChunkHeader(wdtFile, curPos, 'MPHD', sizeof(MPHD));
  curPos += 8;

  mphd.flags = 0;
  mphd.something = 0;
  if (mHasAGlobalWMO)
      mphd.flags |= FLAG_GLOBAL_OBJECT;
  if (mBigAlpha)
      mphd.flags |= FLAG_BIG_ALPHA;
  if (_sort_models_by_size_class)
      mphd.flags |= FLAG_DOODADS_SORT;

  mphd.flags |= FLAG_SHADING;

  wdtFile.Insert(curPos, sizeof(MPHD), (char*)&mphd);
  curPos += sizeof(MPHD);

  // MAIN
  //  {
  wdtFile.Extend(8);
  SetChunkHeader(wdtFile, curPos, 'MAIN', 64 * 64 * 8);
  curPos += 8;

  for (int j = 0; j < 64; ++j)
  {
    for (int i = 0; i < 64; ++i)
    {
      wdtFile.Insert(curPos, 4, (char*)&mTiles[j][i].flags);
      wdtFile.Extend(4);
      curPos += 8;
    }
  }
  //  }

  if (mHasAGlobalWMO)
  {
    // MWMO
    //  {
    // the game requires the path to be zero terminated!
    if(globalWMOName[globalWMOName.size() - 1] != '\0')
    {
        globalWMOName += '\0';
    }
    wdtFile.Extend(8);
    SetChunkHeader(wdtFile, curPos, 'MWMO', static_cast<int>(globalWMOName.size()));
    curPos += 8;

    wdtFile.Insert(curPos, static_cast<unsigned long>(globalWMOName.size()), globalWMOName.data());
    curPos += static_cast<int>(globalWMOName.size());
    //  }

    // MODF
    //  {
    wdtFile.Extend(8);
    SetChunkHeader(wdtFile, curPos, 'MODF', sizeof(ENTRY_MODF));
    curPos += 8;

    auto entry = wmoEntry;
    math::to_server(entry.pos);
    wdtFile.Insert(curPos, sizeof(ENTRY_MODF), (char*)&entry);
    curPos += sizeof(ENTRY_MODF);
    //  }
  }

  // Was BlizzardArchive::ClientFile::save(), which truncated the real WDT at open() and never
  // checked the stream afterwards (ClientFile.cpp:139-150). Losing the WDT is worse than losing
  // one ADT: it is the map's tile table, so a truncated one makes every tile of the map
  // unreachable at once. Same bytes as before -- wdtFile.all_data() is what setBuffer was given.
  BlizzardArchive::Listfile::FileKey const file_key (filename.str());

  std::filesystem::path const target
    ( Noggit::Application::NoggitApplication::instance()->clientData()->getDiskPath (file_key) );

  if (!Noggit::writeFileGuarded (target, wdtFile.all_data()))
  {
    // `changed` deliberately stays set, so that the next save attempt writes the WDT again rather
    // than believing the tile table on disk matches the one in memory.
    return false;
  }

  changed = false;

  return true;
}

void MapIndex::enterTile(const TileIndex& tile)
{
  if (!hasTile(tile))
  {
    return;
  }

  int cx = static_cast<int>(tile.x);
  int cz = static_cast<int>(tile.z);

  for (int pz = std::max(cz - _loading_radius, 0); pz <= std::min(cz + _loading_radius, 63); ++pz)
  {
      for (int px = std::max(cx - _loading_radius, 0); px <= std::min(cx + _loading_radius, 63); ++px)
    {
      loadTile(TileIndex(px, pz));
    }
  }
}

void MapIndex::update_model_tile( const TileIndex& tile
                                , model_update type
                                , SceneObject* instance
                                , tile_dirty_intent intent
                                )
{
  MapTile* adt = loadTile(tile);

  if (!adt)
    return;

  adt->wait_until_loaded();

  // The mark used to be unconditional, and that is the memory leak. unloadTiles below refuses to
  // release any tile whose `changed` flag is set, so every path that reaches this function pinned
  // its tiles for the rest of the session -- including the two that are not edits at all:
  // world_model_instances_storage queues a model_update::add for every model of a tile that is
  // reloading, and for every model whose uid collided and had to be renumbered as it loaded. Both
  // fire from inside MapTile::finishLoading, so on a map with duplicate uids -- the normal state
  // of a custom map that has never had a uid fix run on it -- simply flying across the map marked
  // every tile it streamed in and nothing was ever unloaded again.
  //
  // The flag is still set unconditionally for a user_edit, and that matters: World::
  // snap_selected_models_to_the_ground issues a bare model_update::add with no preceding remove
  // and nothing else marks its tiles, so anything cleverer here -- "only mark when the tile's uid
  // set actually changed", for instance -- would silently drop a snap-to-ground.
  if (intent == tile_dirty_intent::user_edit)
  {
    adt->changed = true;
  }

  if (type == model_update::add)
  {
    adt->add_model(instance);
  }
  else if(type == model_update::remove)
  {
    adt->remove_model(instance);
  }
}

void MapIndex::setChanged(const TileIndex& tile)
{
  MapTile* mTile = loadTile(tile);

  if (!!mTile)
  {
    mTile->changed = true;
  }
}

void MapIndex::setChanged(MapTile* tile)
{
  setChanged(tile->index);
}

void MapIndex::unsetChanged(const TileIndex& tile)
{
  // change the changed flag of the map tile
  if (hasTile(tile))
  {
    // Null-checked because hasTile() above is only a WDT flag test (flags & 1), NOT a statement
    // that the tile is loaded or that its ADT exists. World::for_tile_at_force guards saveTile
    // with `if (tile)` and then calls markOnDisc/unsetChanged/unloadTile unconditionally, so a WDT
    // that lists a tile whose ADT was never written -- the ordinary shape of a custom or WMO-only
    // map, and the same shape that used to crash the UID scan -- arrives here with a null tile.
    MapTile* const map_tile = mTiles[tile.z][tile.x].tile.get();

    if (!map_tile)
    {
      return;
    }

    // Refuses to call a tile clean when the last attempt to write its ADT failed. Every caller of
    // this reaches it straight after a saveTile that used to be unable to report anything, so
    // "we just saved it" was an assumption rather than a fact.
    if (map_tile->lastSaveFailed())
    {
      return;
    }

    map_tile->changed = false;
  }
}

void MapIndex::pinTile(const TileIndex& tile)
{
  // Deliberately does not load the tile: a pin is a promise not to delete something that is
  // already there, and there is nothing to protect in a tile nobody has loaded.
  if (tileLoaded(tile))
  {
    mTiles[tile.z][tile.x].tile->pin();
  }
}

void MapIndex::unpinTile(const TileIndex& tile)
{
  if (tileLoaded(tile))
  {
    mTiles[tile.z][tile.x].tile->unpin();
  }
}

bool MapIndex::isTilePinned(const TileIndex& tile) const
{
  return tileLoaded(tile) && mTiles[tile.z][tile.x].tile->pinned();
}

bool MapIndex::has_unsaved_changes(const TileIndex& tile) const
{
  return (tileLoaded(tile) ? getTile(tile)->changed.load() : false);
}

void MapIndex::setFlag(bool to, glm::vec3 const& pos, uint32_t flag)
{
  TileIndex tile(pos);

  if (tileLoaded(tile))
  {
    setChanged(tile);

    int cx = (pos.x - tile.x * TILESIZE) / CHUNKSIZE;
    int cz = (pos.z - tile.z * TILESIZE) / CHUNKSIZE;

    MapChunk* chunk = getTile(tile)->getChunk(cx, cz);
    NOGGIT_CUR_ACTION->registerChunkFlagChange(chunk);
    chunk->setFlag(to, flag);
  }
}

MapTile* MapIndex::loadTile(const TileIndex& tile, bool reloading, bool load_models, bool load_textures)
{
  if (!hasTile(tile))
  {
    return nullptr;
  }

  if (tileLoaded(tile) || tileAwaitingLoading(tile))
  {
    return mTiles[tile.z][tile.x].tile.get();
  }

  std::stringstream filename;
  filename << "World\\Maps\\" << basename << "\\" << basename << "_" << tile.x << "_" << tile.z << ".adt";

  if (!Noggit::Application::NoggitApplication::instance()->clientData()->exists(filename.str()))
  {
    LogError << "The requested tile \"" << filename.str() << "\" does not exist! Oo" << std::endl;
    return nullptr;
  }

  mTiles[tile.z][tile.x].tile = std::make_unique<MapTile> (static_cast<int>(tile.x), static_cast<int>(tile.z), filename.str(),
     mBigAlpha, load_models, use_mclq_green_lava(), reloading, _world, _context, tile_mode::edit, load_textures);

  MapTile* adt = mTiles[tile.z][tile.x].tile.get();

  AsyncLoader::instance->queue_for_load(adt);
  _n_loaded_tiles++;

  return adt;
}

void MapIndex::reloadTile(const TileIndex& tile)
{
  if (tileLoaded(tile))
  {
    mTiles[tile.z][tile.x].tile.reset();
    loadTile(tile, true);
  }
}

void MapIndex::unloadTiles(const TileIndex& tile)
{
  if (((clock() / CLOCKS_PER_SEC) - _last_unload_time) > _unload_interval)
  {
    // ensure _unload_dist is always bigger than loading dist
    if (_unload_dist <= _loading_radius)
    {
        _unload_dist = _loading_radius + 1;
        QSettings settings;
        settings.setValue("unload_dist", _unload_dist);
        settings.sync();
    }

    unsigned retained_unsaved = 0;
    unsigned retained_pinned = 0;

    for (MapTile* adt : loaded_tiles())
    {
      if (tile.dist(adt->index) > _unload_dist)
      {
        // Two separate reasons to keep a tile, and they are not the same thing. `changed` means
        // the tile holds edits that are not on disk, so dropping it would throw work away.
        // pinned() means some operation is holding raw MapTile*/MapChunk* pointers into it across
        // something that pumps the event loop -- an ambient occlusion bake, a ground effect set
        // pass -- so dropping it would be a use-after-free. A pinned tile is not dirty and is
        // released again the moment the operation lets go.
        if (adt->changed.load())
        {
          ++retained_unsaved;
        }
        else if (adt->pinned())
        {
          ++retained_pinned;
        }
        else
        {
          unloadTile(adt->index);
        }
      }
    }

    // Reported because the numbers are the difference between "the editor is streaming normally"
    // and "this session is going to run out of memory", and nothing else in the editor shows it.
    // MapTile::_chunk_heightmap_buffer alone is std::array<float, 145 * 256 * 4>, which is
    // 145 * 256 * 4 * 4 = 593,920 bytes of the CPU-side copy per retained tile, before any
    // alphamap, texture or GL buffer; the count is the number that matters. Logged only when
    // something was actually held back, so a healthy flight prints nothing.
    if (retained_unsaved || retained_pinned)
    {
      Log << "Tile unload pass kept " << retained_unsaved << " tile(s) with unsaved changes and "
          << retained_pinned << " pinned tile(s) out of " << _n_loaded_tiles << " loaded."
          << std::endl;
    }

    _last_unload_time = clock() / CLOCKS_PER_SEC;
  }
}

void MapIndex::unloadTile(const TileIndex& tile)
{
  // unloads a tile with given cords
  if (tileLoaded(tile))
  {
    // A third reason to keep a tile, alongside the `changed` and pinned() cases in unloadTiles
    // above. Unlike that sweep, this function releases whatever it is given, and the six export
    // paths in World.cpp call it right after unsetChanged and a saveTile whose result they ignore
    // (World.cpp:2249-2255, 3310-3317, 4358-4364, 4421-4427, 4479-4485, 4531-4537). Without this
    // check, an ADT that could not be written would be followed immediately by the destruction of
    // the only remaining copy of those edits -- the file survives the failed write now, but the
    // work done since the last good save would not.
    //
    // Costs nothing in normal use: lastSaveFailed() is false for every tile that has never been
    // saved, which includes every tile the renderer streams out, and it goes false again as soon
    // as a save succeeds. The price of being wrong here is one retained tile of memory; the price
    // of the other answer is the user's session.
    if (mTiles[tile.z][tile.x].tile->lastSaveFailed())
    {
      Log << "Keeping tile " << tile.x << "-" << tile.z
          << " loaded: its last save failed, so unloading it would discard the only copy of its "
             "changes." << std::endl;

      return;
    }

    // either log before or don't use a reference for the tile/make a copy
    // otherwise it can be deleted before the log because it comes from the adt itself (see unloadTiles)
    Log << "Unloading Tile " << tile.x << "-" << tile.z << std::endl;

    AsyncLoader::instance->ensure_deletable(mTiles[tile.z][tile.x].tile.get());
    mTiles[tile.z][tile.x].tile.reset();
    _n_loaded_tiles--;

    // Drop this tile's composited mask blocks along with the tile itself. This call is what
    // bounds mask memory: a composited mask block is a dense 64x64 bytes per chunk, so 4096
    // bytes a chunk and 1,048,576 bytes for a fully dense tile. Nothing else ever frees a baked
    // tile, so without this the field would only grow as the camera moved.
    //
    // Hooked HERE rather than inside the unloadTiles sweep, because this function is the single
    // choke point every unload passes through -- both the distance sweep and the "unload any
    // previously loaded tile" pass in setChanged call it -- and hooking the sweep would have
    // missed the second one.
    Noggit::TerrainMaskStore::instance()->releaseTile(tile.x, tile.z);
  }
}

void MapIndex::markOnDisc(const TileIndex& tile, bool mto)
{
  if(tile.is_valid())
  {
    // The third and last member of the sequence the six World.cpp export paths run after a
    // saveTile whose result they ignore: markOnDisc, unsetChanged, unloadTile. The other two now
    // consult lastSaveFailed(); without this one, a tile whose first-ever write failed would be
    // recorded as present on disc when no file exists there, and isTileExternal() would then
    // answer yes for a path that cannot be read.
    //
    // Only the true direction is refused. Marking a tile as NOT on disc is always safe and is how
    // a deletion is recorded, so it must never be blocked.
    if (mto && tileLoaded(tile) && mTiles[tile.z][tile.x].tile->lastSaveFailed())
    {
      return;
    }

    mTiles[tile.z][tile.x].onDisc = mto;
  }
}

bool MapIndex::isTileExternal(const TileIndex& tile) const
{
  // is onDisc
  return tile.is_valid() && mTiles[tile.z][tile.x].onDisc;
}

bool MapIndex::saveTile(const TileIndex& tile, World* world, bool save_unloaded)
{
  world->wait_for_all_tile_updates();

	// save given tile
	if (save_unloaded)
  {
    // The QFile that used to stand here opened the destination ADT with QIODevice::WriteOnly,
    // which truncates it, wrote nothing at all, and let the handle close at the end of the
    // statement's scope. MapTile::saveTile then produced the real file. So the tile on disk was
    // destroyed a measurable interval before its replacement even began to be built, and any
    // failure in between -- including the two "filename changed during save" bail-outs in
    // MapTile::save -- left a zero-byte ADT. Nothing depended on the file existing first: the
    // writer creates its own parent directories, and the ClientFile constructor used for a new
    // file never reads the destination.
    mTiles[tile.z][tile.x].tile->initEmptyChunks();

    return mTiles[tile.z][tile.x].tile->saveTile(world);
  }

	if (tileLoaded(tile))
	{
    saveMaxUID();
    world->horizon.update_horizon_tile(mTiles[tile.z][tile.x].tile.get());

    // `changed` is deliberately left exactly as this function found it. It never cleared the flag
    // before either, and that direction of the asymmetry is safe -- a tile wrongly believed dirty
    // is only kept in memory and re-saved, whereas a tile wrongly believed clean is released and
    // lost. Making it symmetric would let unloadTiles release a tile it currently keeps, which is
    // a lifetime change and not a write-safety one.
    return mTiles[tile.z][tile.x].tile->saveTile(world);
	}

  return true;
}

// Continue-and-report, for the reasons argued above MapIndex::saveall.
bool MapIndex::saveChanged (World* world, bool save_unloaded)
{
  world->wait_for_all_tile_updates();

  bool all_saved (true);

  if (changed)
  {
    // Not an early return even though this is the map's tile table. The ADTs are self-contained
    // files; a WDT that could not be replaced does not make the ADT that CAN be replaced any less
    // worth writing, and the old WDT still on disk describes the same 64x64 grid unless tiles were
    // added or removed this session.
    all_saved = save();
  }

  if (!save_unloaded)
  {
    saveMaxUID();
  }
  else
  {
    for (int i = 0; i < 64; ++i)
    {
      for (int j = 0; j < 64; ++j)
      {
        if (!(mTiles[i][j].tile && mTiles[i][j].tile->changed.load()))
        {
          continue;
        }

        auto filepath = std::filesystem::path (Noggit::Project::CurrentProject::get()->ProjectPath)
                        / BlizzardArchive::ClientData::normalizeFilenameInternal (mTiles[i][j].tile->file_key().filepath());

        if (mTiles[i][j].flags & 0x1)
        {
          // The QFile that stood here opened the destination ADT WriteOnly -- truncating it to
          // zero bytes -- and wrote nothing, leaving MapTile::saveTile to produce the real file
          // afterwards. See MapIndex::saveTile for the same removal and the same reasoning.
          mTiles[i][j].tile->initEmptyChunks();

          if (mTiles[i][j].tile->saveTile(world))
          {
            mTiles[i][j].tile->changed = false;
          }
          else
          {
            all_saved = false;
          }
        }
        else
        {
          // Deliberate deletion: the WDT no longer flags this tile as present, so the stale ADT
          // has to go. Checked now because a failure here leaves a file the client will still
          // load, and silently disagreeing with the WDT is its own kind of lost work.
          QFile file(filepath.string().c_str());

          if (file.exists() && !file.remove())
          {
            Noggit::reportSaveFailure
              ( filepath
              , "the tile was removed from the map but its ADT could not be deleted: "
                + file.errorString().toStdString()
              );

            all_saved = false;
          }
        }
      }
    }

    return all_saved;
  }

  for (MapTile* tile : loaded_tiles())
  {
    if (tile->changed.load())
    {
      world->horizon.update_horizon_tile(tile);

      // Cleared ONLY on success. This assignment used to be unconditional, next to a saveTile
      // that could not report a failure, so a tile whose ADT had just been truncated was marked
      // clean; unloadTiles was then free to release it, and the edits were gone from the disk and
      // from memory at once. That pair was the largest data-loss path in the program.
      if (tile->saveTile(world))
      {
        tile->changed = false;
      }
      else
      {
        all_saved = false;
      }
    }
  }

  return all_saved;
}

bool MapIndex::hasAGlobalWMO() const
{
  return mHasAGlobalWMO;
}


bool MapIndex::hasTile(const TileIndex& tile) const
{
  return tile.is_valid() && (mTiles[tile.z][tile.x].flags & 1);
}

bool MapIndex::tileAwaitingLoading(const TileIndex& tile) const
{
  return hasTile(tile) && mTiles[tile.z][tile.x].tile && !mTiles[tile.z][tile.x].tile->finishedLoading();
}

bool MapIndex::tileLoaded(const TileIndex& tile) const
{
  return hasTile(tile) && mTiles[tile.z][tile.x].tile && mTiles[tile.z][tile.x].tile->finishedLoading();
}

MapTile* MapIndex::getTile(const TileIndex& tile) const
{
  return (tile.is_valid() ? mTiles[tile.z][tile.x].tile.get() : nullptr);
}

MapTile* MapIndex::getTileAbove(MapTile* tile) const
{
  TileIndex above(tile->index.x, tile->index.z - 1);
  if (tile->index.z == 0 || (!tileLoaded(above) && !tileAwaitingLoading(above)))
  {
    return nullptr;
  }

  MapTile* tile_above = mTiles[tile->index.z - 1][tile->index.x].tile.get();
  tile_above->wait_until_loaded();

  return tile_above;
}

MapTile* MapIndex::getTileLeft(MapTile* tile) const
{
  TileIndex left(tile->index.x - 1, tile->index.z);
  if (tile->index.x == 0 || (!tileLoaded(left) && !tileAwaitingLoading(left)))
  {
    return nullptr;
  }

  MapTile* tile_left = mTiles[tile->index.z][tile->index.x - 1].tile.get();
  tile_left->wait_until_loaded();

  return tile_left;
}

uint32_t MapIndex::getFlag(const TileIndex& tile) const
{
  return (tile.is_valid() ? mTiles[tile.z][tile.x].flags : 0);
}

void MapIndex::convert_alphamap(bool to_big_alpha)
{
  mBigAlpha = to_big_alpha;
  if (to_big_alpha)
  {
    mphd.flags |= 4;
  }
  else
  {
    mphd.flags &= 0xFFFFFFFB;
  }
}

bool MapIndex::hasBigAlpha() const
{
  return mBigAlpha;
}

void MapIndex::setBigAlpha(bool state)
{
  mBigAlpha = state;
}

unsigned MapIndex::getNLoadedTiles() const
{
  return _n_loaded_tiles;
}

bool MapIndex::sort_models_by_size_class() const
{
  return _sort_models_by_size_class;
}

void MapIndex::set_sort_models_by_size_class(bool state)
{
  _sort_models_by_size_class = state;
}


std::optional<uint32_t> MapIndex::getHighestGUIDFromFile(const std::string& pFilename) const
{
	uint32_t highGUID = 0;

    auto* const client_data = Noggit::Application::NoggitApplication::instance()->clientData();

    // The isEof() test below never sees a missing file, and that was the crash. The normal
    // BlizzardArchive::ClientFile constructor THROWS Exceptions::FileReadFailedError when the name
    // is in neither the project directory nor an archive (ClientFile.cpp:43-46) -- it never
    // returns an object with _eof set, so the guard underneath it is unreachable for exactly the
    // case it looks like it is handling.
    //
    // That matters because the only caller, searchMaxUID, walks every tile whose WDT MAIN entry
    // has bit 0 set, and a WDT is perfectly able to flag a tile whose ADT was never written --
    // which is the normal state of a custom or WMO-only map. searchMaxUID runs from
    // MapView::initializeGL (MapView.cpp:4941), and paintGL calls initializeGL directly
    // (MapView.cpp:5057), so the exception left a Qt paint event through a live
    // OpenGL::context::scoped_setter and the editor exited with no message at all.
    //
    // Asked before constructing rather than only caught, because exists() is the same test
    // MapIndex::loadTile already uses to skip a flagged-but-absent tile, and a throw per missing
    // tile on a map with hundreds of them is needless. The catch stays anyway: exists() and the
    // constructor reach the client data through two different code paths and a file can also fail
    // to read for reasons other than being absent.
    if (!client_data->exists(pFilename))
    {
      return std::nullopt;
    }

    try
    {
      BlizzardArchive::ClientFile theFile(pFilename, client_data);
      if (theFile.isEof())
      {
        return highGUID;
      }

      uint32_t fourcc;
      uint32_t size;

      MHDR Header;

      // - MVER ----------------------------------------------

      uint32_t version;

      theFile.read(&fourcc, 4);
      theFile.seekRelative(4);
      theFile.read(&version, 4);

      assert(fourcc == 'MVER' && version == 18);

      // - MHDR ----------------------------------------------

      theFile.read(&fourcc, 4);
      theFile.seekRelative(4);

      assert(fourcc == 'MHDR');

      theFile.read(&Header, sizeof(MHDR));

      // - MDDF ----------------------------------------------

      theFile.seek(Header.mddf + 0x14);
      theFile.read(&fourcc, 4);
      theFile.read(&size, 4);

      assert(fourcc == 'MDDF');

      ENTRY_MDDF const* mddf_ptr = reinterpret_cast<ENTRY_MDDF const*>(theFile.getPointer());
      for (unsigned int i = 0; i < size / sizeof(ENTRY_MDDF); ++i)
      {
          highGUID = std::max(highGUID, mddf_ptr[i].uniqueID);
      }

      // - MODF ----------------------------------------------

      theFile.seek(Header.modf + 0x14);
      theFile.read(&fourcc, 4);
      theFile.read(&size, 4);

      assert(fourcc == 'MODF');

      ENTRY_MODF const* modf_ptr = reinterpret_cast<ENTRY_MODF const*>(theFile.getPointer());
      for (unsigned int i = 0; i < size / sizeof(ENTRY_MODF); ++i)
      {
          highGUID = std::max(highGUID, modf_ptr[i].uniqueID);
      }
      theFile.close();

      return highGUID;
    }
    catch (BlizzardArchive::Exceptions::FileReadFailedError const& error)
    {
      LogError << "Skipping \"" << pFilename << "\" while scanning for the highest uid: "
               << error.what() << std::endl;

      return std::nullopt;
    }
}

// reloadable settings
void MapIndex::setLoadingRadius(int value)
{
  if (value < _unload_dist)
    _loading_radius = value;
}

void MapIndex::setUnloadDistance(int value)
{
  if (value > _loading_radius)
    _unload_dist = value;
}

void MapIndex::setUnloadInterval(int value)
{
  _unload_interval = value;
}

uint32_t MapIndex::newGUID()
{
  std::unique_lock<std::mutex> lock (_mutex);

#ifdef USE_MYSQL_UID_STORAGE
  QSettings settings;

  if (settings.value ("project/mysql/enabled", false).toBool())
  {
    mysql::updateUIDinDB(_map_id, highestGUID + 1); // update the highest uid in db, note that if the user don't save these uid won't be used (not really a problem tho) 
  }
#endif
  return ++highestGUID;
}

uid_fix_status MapIndex::fixUIDs (World* world, bool cancel_on_model_loading_error)
{
  // clear all selection groups since UIDs will change.
  // TODO : update them instead.
    _world->clear_selection_groups();

  // pre-cond: mTiles[z][x].flags are set

  _uid_scan_skipped_tiles = 0;

  // unload any previously loaded tile, although there shouldn't be as
  // the fix is executed before loading the map
  for (int z = 0; z < 64; ++z)
  {
    for (int x = 0; x < 64; ++x)
    {
      if (mTiles[z][x].tile)
      {
        MapTile* tile = mTiles[z][x].tile.get();

        // don't unload half loaded tiles
        tile->wait_until_loaded();

        unloadTile(tile->index);
      }
    }
  }

  _uid_fix_all_in_progress = true;

  auto models = std::make_unique<std::forward_list<ModelInstance>>();
  auto wmos = std::make_unique<std::forward_list<WMOInstance>>();

  for (int z = 0; z < 64; ++z)
  {
    for (int x = 0; x < 64; ++x)
    {
      if (!(mTiles[z][x].flags & 1))
      {
        continue;
      }

      std::stringstream filename;
      filename << "World\\Maps\\" << basename << "\\" << basename << "_" << x << "_" << z << ".adt";

      // Same missing-ADT crash as the one described in getHighestGUIDFromFile, in the other uid
      // path: this constructor throws for a tile the WDT flags but that was never written, the
      // isEof() guard below it can therefore never see that case, and fixUIDs is reached from
      // MapView::initializeGL just as searchMaxUID is. A flagged-but-absent tile is skipped and
      // counted; there is nothing in it to renumber.
      if (!Noggit::Application::NoggitApplication::instance()->clientData()->exists(filename.str()))
      {
        ++_uid_scan_skipped_tiles;
        continue;
      }

      BlizzardArchive::ClientFile file(filename.str(), Noggit::Application::NoggitApplication::instance()->clientData());

      if (file.isEof())
      {
        continue;
      }

      std::array<glm::vec3, 2> tileExtents;
      tileExtents[0] = { x*TILESIZE, 0, z*TILESIZE };
      tileExtents[1] = { (x+1)*TILESIZE, 0, (z+1)*TILESIZE };
      misc::minmax(&tileExtents[0], &tileExtents[1]);

      std::forward_list<ENTRY_MDDF> modelEntries;
      std::forward_list<ENTRY_MODF> wmoEntries;
      std::vector<std::string> modelFilenames;
      std::vector<std::string> wmoFilenames;

      uint32_t fourcc;
      uint32_t size;

      MHDR Header;

      // - MVER ----------------------------------------------
      uint32_t version;
      file.read(&fourcc, 4);
      file.seekRelative(4);
      file.read(&version, 4);
      assert(fourcc == 'MVER' && version == 18);

      // - MHDR ----------------------------------------------
      file.read(&fourcc, 4);
      file.seekRelative(4);
      assert(fourcc == 'MHDR');
      file.read(&Header, sizeof(MHDR));

      // - MDDF ----------------------------------------------
      file.seek(Header.mddf + 0x14);
      file.read(&fourcc, 4);
      file.read(&size, 4);
      assert(fourcc == 'MDDF');

      ENTRY_MDDF const* mddf_ptr = reinterpret_cast<ENTRY_MDDF const*>(file.getPointer());

      for (unsigned int i = 0; i < size / sizeof(ENTRY_MDDF); ++i)
      {
        bool add = true;
        ENTRY_MDDF const& mddf = mddf_ptr[i];

        if (!misc::pointInside({ mddf.pos[0], 0, mddf.pos[2] }, tileExtents))
        {
          continue;
        }

        // check for duplicates
        for (ENTRY_MDDF& entry : modelEntries)
        {
          if ( mddf.nameID == entry.nameID
            && misc::float_equals(mddf.pos[0], entry.pos[0])
            && misc::float_equals(mddf.pos[1], entry.pos[1])
            && misc::float_equals(mddf.pos[2], entry.pos[2])
            && misc::float_equals(mddf.rot[0], entry.rot[0])
            && misc::float_equals(mddf.rot[1], entry.rot[1])
            && misc::float_equals(mddf.rot[2], entry.rot[2])
            && mddf.scale == entry.scale
            )
          {
            add = false;
            break;
          }
        }

        if (add)
        {
          modelEntries.emplace_front(mddf);
        }
      }

      // - MODF ----------------------------------------------
      file.seek(Header.modf + 0x14);
      file.read(&fourcc, 4);
      file.read(&size, 4);
      assert(fourcc == 'MODF');

      ENTRY_MODF const* modf_ptr = reinterpret_cast<ENTRY_MODF const*>(file.getPointer());

      for (unsigned int i = 0; i < size / sizeof(ENTRY_MODF); ++i)
      {
        bool add = true;
        ENTRY_MODF const& modf = modf_ptr[i];

        if (!misc::pointInside({ modf.pos[0], 0, modf.pos[2] }, tileExtents))
        {
          continue;
        }

        // check for duplicates
        for (ENTRY_MODF& entry : wmoEntries)
        {
          if (modf.nameID == entry.nameID
            && misc::float_equals(modf.pos[0], entry.pos[0])
            && misc::float_equals(modf.pos[1], entry.pos[1])
            && misc::float_equals(modf.pos[2], entry.pos[2])
            && misc::float_equals(modf.rot[0], entry.rot[0])
            && misc::float_equals(modf.rot[1], entry.rot[1])
            && misc::float_equals(modf.rot[2], entry.rot[2])
            )
          {
            add = false;
            break;
          }
        }

        if (add)
        {
          wmoEntries.emplace_front(modf);
        }
      }

      // - MMDX ----------------------------------------------
      file.seek(Header.mmdx + 0x14);
      file.read(&fourcc, 4);
      file.read(&size, 4);
      assert(fourcc == 'MMDX');

      {
        char const* lCurPos = reinterpret_cast<char const*>(file.getPointer());
        char const* lEnd = lCurPos + size;

        while (lCurPos < lEnd)
        {
          modelFilenames.push_back(std::string(lCurPos));
          lCurPos += strlen(lCurPos) + 1;
        }
      }

      // - MWMO ----------------------------------------------
      file.seek(Header.mwmo + 0x14);
      file.read(&fourcc, 4);
      file.read(&size, 4);
      assert(fourcc == 'MWMO');

      {
        char const* lCurPos = reinterpret_cast<char const*>(file.getPointer());
        char const* lEnd = lCurPos + size;

        while (lCurPos < lEnd)
        {
          wmoFilenames.push_back(std::string(lCurPos));
          lCurPos += strlen(lCurPos) + 1;
        }
      }

      file.close();

      for (ENTRY_MDDF& entry : modelEntries)
      {
        models->emplace_front(modelFilenames[entry.nameID], &entry, _context);
      }
      for (ENTRY_MODF& entry : wmoEntries)
      {
        wmos->emplace_front(wmoFilenames[entry.nameID], &entry, _context);
      }
    }
  }

  // set all uids
  // for each tile save the m2/wmo present inside
  highestGUID = 0;

  auto uids_per_tile = std::make_unique<std::map<std::size_t, std::map<std::size_t, std::forward_list<std::uint32_t>>>>();

  bool loading_error = false;

  for (ModelInstance& instance : *models)
  {
    instance.uid = highestGUID++;
    instance.model->wait_until_loaded();
    instance.recalcExtents();

    loading_error |= instance.model->loading_failed();

    // to avoid going outside of bound
    std::size_t sx = std::max((std::size_t)(instance.getExtents()[0].x / TILESIZE), (std::size_t)0);
    std::size_t sz = std::max((std::size_t)(instance.getExtents()[0].z / TILESIZE), (std::size_t)0);
    std::size_t ex = std::min((std::size_t)(instance.getExtents()[1].x / TILESIZE), (std::size_t)63);
    std::size_t ez = std::min((std::size_t)(instance.getExtents()[1].z / TILESIZE), (std::size_t)63);

    auto const real_uid (world->add_model_instance (std::move(instance), false, false));

    for (std::size_t z = sz; z <= ez; ++z)
    {
      auto& row_map = (*uids_per_tile)[z];
      for (std::size_t x = sx; x <= ex; ++x)
      {
          auto& uid_list = row_map[x];
          uid_list.emplace_front(real_uid);
      }
    }
  }

  models.reset();

  for (WMOInstance& instance : *wmos)
  {
    instance.uid = highestGUID++;
    instance.wmo->wait_until_loaded();
    instance.recalcExtents();
    // no need to check if the loading is finished since the extents are stored inside the adt
    // to avoid going outside of bound
    std::size_t sx = std::max((std::size_t)(instance.getExtents()[0].x / TILESIZE), (std::size_t)0);
    std::size_t sz = std::max((std::size_t)(instance.getExtents()[0].z / TILESIZE), (std::size_t)0);
    std::size_t ex = std::min((std::size_t)(instance.getExtents()[1].x / TILESIZE), (std::size_t)63);
    std::size_t ez = std::min((std::size_t)(instance.getExtents()[1].z / TILESIZE), (std::size_t)63);

    auto const real_uid (world->add_wmo_instance (std::move(instance), false, false));

    for (std::size_t z = sz; z <= ez; ++z)
    {
      auto& row_map = (*uids_per_tile)[z];
      for (std::size_t x = sx; x <= ex; ++x)
      {
        auto& uid_list = row_map[x];
        uid_list.emplace_front(real_uid);
      }
    }
  }

  wmos.reset();

  if (cancel_on_model_loading_error && loading_error)
  {
    return uid_fix_status::failed;
  }

  // load each tile without the models and
  // save them with the models with the new uids
  for (int z = 0; z < 64; ++z)
  {
    for (int x = 0; x < 64; ++x)
    {
      if (!(mTiles[z][x].flags & 1))
      {
        continue;
      }

      // load even the tiles without models in case there are old ones
      // that shouldn't be there to avoid creating new duplicates

      std::stringstream filename;
      filename << "World\\Maps\\" << basename << "\\" << basename << "_" << x << "_" << z << ".adt";

      // Third instance of the same missing-ADT crash, and the one the first two would have hidden:
      // MapTile::finishLoading opens the file with the same throwing constructor (MapTile.cpp:119).
      // MapIndex::loadTile already refuses a flagged-but-absent tile with exactly this exists()
      // test before it builds a MapTile; this loop builds one directly and did not.
      if (!Noggit::Application::NoggitApplication::instance()->clientData()->exists(filename.str()))
      {
        continue;
      }

      // load the tile without the models
      MapTile tile(x, z, filename.str(), mBigAlpha, false, use_mclq_green_lava(), false, world, _context, tile_mode::uid_fix_all);
      tile.finishLoading();

      // add the uids to the tile to be able to save the models
      // which have been loaded in world earlier
      for (std::uint32_t uid : (*uids_per_tile)[z][x])
      {
        tile.add_model(uid);
      }

      tile.saveTile(world);
    }
  }

  if (_uid_scan_skipped_tiles)
  {
    LogError << "Uid fix: " << _uid_scan_skipped_tiles
             << " tile(s) are flagged in the WDT but have no ADT file and were skipped."
             << std::endl;
  }

  // override the db highest uid if used
  saveMaxUID();

  _uid_fix_all_in_progress = false;

  // force instances unloading
  world->unload_every_model_and_wmo_instance();

  return loading_error ? uid_fix_status::done_with_errors : uid_fix_status::done;
}

void MapIndex::searchMaxUID()
{
  _uid_scan_skipped_tiles = 0;

  for (int z = 0; z < 64; ++z)
  {
    for (int x = 0; x < 64; ++x)
    {
      if (!(mTiles[z][x].flags & 1))
      {
        continue;
      }

      std::stringstream filename;
      filename << "World\\Maps\\" << basename << "\\" << basename << "_" << x << "_" << z << ".adt";

      // A flagged tile with no file is skipped and the scan carries on, where it used to take the
      // whole editor down: getHighestGUIDFromFile constructed a ClientFile, that constructor threw
      // for the missing file, and the exception left MapView::initializeGL from inside paintGL.
      //
      // Carrying on rather than stopping is the right answer even though the result is then an
      // underestimate of the highest uid in the map: every tile that IS present still contributes,
      // so the maximum can only be too low by whatever the absent tiles held -- and an absent tile
      // holds nothing. The count is kept so MapView can tell the user the scan was incomplete.
      if (auto const tile_max = getHighestGUIDFromFile(filename.str()))
      {
        highestGUID = std::max(highestGUID, *tile_max);
      }
      else
      {
        ++_uid_scan_skipped_tiles;
      }
    }
  }

  if (_uid_scan_skipped_tiles)
  {
    LogError << "Highest uid scan: " << _uid_scan_skipped_tiles
             << " tile(s) are flagged in the WDT but have no ADT file and were skipped."
             << std::endl;
  }

  saveMaxUID();
}

unsigned MapIndex::uidScanSkippedTiles() const
{
  return _uid_scan_skipped_tiles;
}

void MapIndex::saveMaxUID()
{
#ifdef USE_MYSQL_UID_STORAGE
  QSettings settings;

  if (settings.value ("project/mysql/enabled", false).toBool())
  {
    if (mysql::hasMaxUIDStoredDB(_map_id))
    {
	    mysql::updateUIDinDB(_map_id, highestGUID);
    }
    else
    {
	    mysql::insertUIDinDB(_map_id, highestGUID);
    }
  }
#endif
  // save the max UID on the disk (always save to sync with the db if used
  uid_storage::saveMaxUID (_map_id, highestGUID);
}

void MapIndex::loadMaxUID()
{
  highestGUID = uid_storage::getMaxUID (_map_id);
#ifdef USE_MYSQL_UID_STORAGE
  QSettings settings;

  if (settings.value ("project/mysql/enabled", false).toBool())
  {
    highestGUID = std::max(mysql::getGUIDFromDB(_map_id), highestGUID);
    // save to make sure the db and disk uid are synced
    saveMaxUID();
  }
#endif
}

void MapIndex::loadMinimapMD5translate()
{
  auto& minimap_md5translate = Noggit::Application::NoggitApplication::instance()->clientData()->_minimap_md5translate;

  // already loaded.
  if (minimap_md5translate.empty())
    return;

  if (!Noggit::Application::NoggitApplication::instance()->clientData()->exists("textures/minimap/md5translate.trs"))
  {
    LogError << "md5translate.trs was not found. "
                "Noggit will generate a new one in the project directory on minimap save." << std::endl;
    return;
  }

  BlizzardArchive::ClientFile md5trs_file("textures/minimap/md5translate.trs", Noggit::Application::NoggitApplication::instance()->clientData());

  size_t size = md5trs_file.getSize();
  void* buffer_raw = std::malloc(size);
  md5trs_file.read(buffer_raw, size);

  QByteArray md5trs_bytes(static_cast<char*>(buffer_raw), static_cast<int>(size));

  QTextStream md5trs_stream(md5trs_bytes, QIODevice::ReadOnly);

  QString cur_dir = "";
  while (!md5trs_stream.atEnd())
  {
    QString line = md5trs_stream.readLine();

    if (!line.length())
    {
      continue;
    }

    if (line.startsWith("dir: ", Qt::CaseInsensitive))
    {
      QStringList dir_line_split = line.split(" ");
      cur_dir = dir_line_split[1];
      continue;
    }

    QStringList line_split = line.split(QRegExp("[\t]"));

    if (line_split.length() < 2)
    {
        std::string text = "Failed to read md5translate.trs.\nLine \"" + line.toStdString() + "\n has no tab spacing. Spacing must be only a tab character and not spaces.";
        LogError << text << std::endl;
        throw  std::logic_error(text);
    }

    if (cur_dir.length())
    {
      minimap_md5translate[cur_dir.toStdString()][line_split[0].toStdString()] = line_split[1].toStdString();
    }

  }

}

void MapIndex::saveMinimapMD5translate()
{
  QString str = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
  if (!(str.endsWith('\\') || str.endsWith('/')))
  {
    str += "/";
  }

  QString filepath = str + "/textures/minimap/md5translate.trs";

  // Written to a sibling and renamed, like every other save in this file. md5translate.trs is the
  // whole map's minimap index: truncate it and every minimap tile becomes unfindable at once, and
  // the file is rebuilt only by re-rendering the minimaps.
  //
  // Unlike the ADT and WDT writers this one keeps its own QFile and QTextStream instead of handing
  // a buffer to writeFileAtomically, because QIODevice::Text rewrites every "\n" as "\r\n" on
  // Windows. Reproducing that by hand would be a change to WHAT is written; pointing the identical
  // stream at the temp file is not.
  QString const temporary_path = filepath + ".tmp";

  QFile file = QFile(temporary_path);
  if (file.open(QIODevice::WriteOnly | QIODevice::Text | QFile::Truncate))
  {
    // Both halves are needed. QTextStream::status() is the only thing that remembers a write that
    // failed part way through, because QFileDevice::close() calls unsetError() when its own final
    // flush and close both succeed and would erase that history; QFile::error() is the only thing
    // that sees a failure in that final flush or close. Neither alone covers a full disk.
    bool text_ok (false);

    {
      QTextStream out(&file);

      auto const& minimap_md5translate = Noggit::Application::NoggitApplication::instance()->clientData()->_minimap_md5translate;

      for (auto it = minimap_md5translate.begin(); it != minimap_md5translate.end(); ++it)
      {
        out << "dir: " << it->first.c_str() << "\n"; // save dir

        for (auto it_ = it->second.begin(); it_ != it->second.end(); ++it_)
        {
          out << it_->first.c_str() << "\t" << it_->second.c_str() << "\n";
        }
      }

      // The QTextStream buffers, so it has to be flushed into the QFile before the QFile is
      // closed and questioned. Scoping it here is what guarantees the order.
      out.flush();

      text_ok = (out.status() == QTextStream::Ok);
    }

    file.close();

    // Asked after close() for the same reason DBCFile::save asks after close(): a full disk is
    // reported at the final flush, not at any individual write.
    if (!text_ok || file.error() != QFileDevice::NoError)
    {
      QString const message = file.errorString();
      file.remove();

      Noggit::reportSaveFailure
        ( std::filesystem::path (filepath.toStdString())
        , "writing the minimap index failed: " + message.toStdString()
        );
    }
    else
    {
      std::string commit_error;

      if (!Noggit::commitReplacementFile ( std::filesystem::path (temporary_path.toStdString())
                                         , std::filesystem::path (filepath.toStdString())
                                         , &commit_error
                                         )
         )
      {
        Noggit::reportSaveFailure (std::filesystem::path (filepath.toStdString()), commit_error);
      }
    }
  }
  else
  {
    LogError << "Failed saving md5translate.trs. File can't be opened." << std::endl;

    // The old code stopped at that log line. Losing the minimap index without being told means
    // discovering it the next time the map is opened, with nothing left to recover from.
    Noggit::reportSaveFailure
      ( std::filesystem::path (filepath.toStdString())
      , "could not open " + temporary_path.toStdString() + " for writing: "
        + file.errorString().toStdString()
      );
  }
}

void MapIndex::addTile(const TileIndex& tile)
{
  std::stringstream filename;
  filename << "World\\Maps\\" << basename << "\\" << basename << "_" << tile.x << "_" << tile.z << ".adt";

  mTiles[tile.z][tile.x].tile = std::make_unique<MapTile> (static_cast<int>(tile.x), static_cast<int>(tile.z), filename.str(),
      mBigAlpha, true, use_mclq_green_lava(), false, _world, _context);

  mTiles[tile.z][tile.x].flags |= 0x1;
  mTiles[tile.z][tile.x].tile->changed = true;

  _world->horizon.update_horizon_tile(mTiles[tile.z][tile.x].tile.get());

  changed = true;
}

void MapIndex::removeTile(const TileIndex &tile)
{
  mTiles[tile.z][tile.x].flags &= ~0x1;

  std::stringstream filename;
  filename << "World\\Maps\\" << basename << "\\" << basename << "_" << tile.x << "_" << tile.z << ".adt";
  mTiles[tile.z][tile.x].tile = std::make_unique<MapTile> (static_cast<int>(tile.x), static_cast<int>(tile.z), filename.str(),
     mBigAlpha, true, use_mclq_green_lava(), false, _world, _context);

  mTiles[tile.z][tile.x].tile->changed = true;
  mTiles[tile.z][tile.x].onDisc = false;

  _world->horizon.remove_horizon_tile(tile.z, tile.x);

  changed = true;
}

void MapIndex::addGlobalWmo(std::string path, ENTRY_MODF entry)
{
    mHasAGlobalWMO = true;
    globalWMOName = std::move(path);
    wmoEntry = std::move(entry);
}

void MapIndex::removeGlobalWmo()
{
    mHasAGlobalWMO = false;
    globalWMOName.clear();
    wmoEntry = {};
}

unsigned MapIndex::getNumExistingTiles()
{
  if (_n_existing_tiles >= 0)
    return _n_existing_tiles;

  _n_existing_tiles = 0;
  for (int i = 0; i < 4096; ++i)
  {
    TileIndex index(i / 64, i % 64);

    if (hasTile(index))
    {
      _n_existing_tiles++;
    }
  }

  return _n_existing_tiles;
}

// todo: find out how wow choose to use the green lava in outland
bool MapIndex::use_mclq_green_lava() const
{
  return _map_id == 530;
}

bool MapIndex::uid_fix_all_in_progress() const
{
  return _uid_fix_all_in_progress;
}

void MapIndex::set_basename(const std::string &pBasename)
{
  basename = pBasename;

  for (int z = 0; z < 64; ++z)
  {
    for (int x = 0; x < 64; ++x)
    {
      if (!mTiles[z][x].tile)
      {
        continue;
      }

      std::stringstream filename;
      filename << "World\\Maps\\" << basename << "\\" << basename << "_" << x << "_" << z << ".adt";

      mTiles[z][x].tile->setFilename(filename.str());
    }
  }
}

void MapIndex::create_empty_wdl() const
{
    // for new map creation, creates a new WDL with all heights as 0
    std::stringstream filename;
    filename << "World\\Maps\\" << basename << "\\" << basename << ".wdl"; // mapIndex.basename ? 
    //Log << "Saving WDL \"" << filename << "\"." << std::endl;

    util::sExtendableArray wdlFile;
    int curPos = 0;

    // MVER
    //  {
    wdlFile.Extend(8 + 0x4);
    SetChunkHeader(wdlFile, curPos, 'MVER', 4);

    // MVER data
    *(wdlFile.GetPointer<int>(8)) = 18; // write version 18
    curPos += 8 + 0x4;
    //  }

    // MWMO
    //  {
    wdlFile.Extend(8);
    SetChunkHeader(wdlFile, curPos, 'MWMO', 0);

    curPos += 8;
    //  }

    // MWID
    //  {
    wdlFile.Extend(8);
    SetChunkHeader(wdlFile, curPos, 'MWID', 0);

    curPos += 8;
    //  }

    // MODF
    //  {
    wdlFile.Extend(8);
    SetChunkHeader(wdlFile, curPos, 'MODF', 0);

    curPos += 8;
    //  }

    uint32_t* mare_offsets = new uint32_t[4096]();
    // uint32_t mare_offsets[4096] = { 0 }; // [64][64];
    // MAOF
    //  {
    wdlFile.Extend(8);
    SetChunkHeader(wdlFile, curPos, 'MAOF', 64 * 64 * 4);
    curPos += 8;

    uint32_t mareoffset = curPos + 64 * 64 * 4;

    for (int y = 0; y < 64; ++y)
    {
        for (int x = 0; x < 64; ++x)
        {
            TileIndex index(x, y);

            bool has_tile = hasTile(index);

            // if (tile_exists)
            if (has_tile) // TODO check if tile exists
            {
                // write offset in MAOF entry
                wdlFile.Insert(curPos, 4, (char*)&mareoffset);
                mare_offsets[y * 64 + x] = mareoffset;
                mareoffset += 1138; // mare + maho
            }
            else
                wdlFile.Extend(4);
            curPos += 4;

        }
    }

    for (int i = 0; i < 4096; ++i)
    {
        uint32_t offset = mare_offsets[i];
        if (!offset)
            continue;

        // MARE
        //  {
        wdlFile.Extend(8);
        SetChunkHeader(wdlFile, curPos, 'MARE', (2 * (17 * 17)) + (2 * (16 * 16))); // outer heights+inner heights
        curPos += 8;

        // write inner and outer heights
        wdlFile.Extend((2 * (17 * 17)) + (2 * (16 * 16)));
        curPos += (2 * (17 * 17)) + (2 * (16 * 16));
        //  }

        // MAHO (maparea holes)
        //  {
        wdlFile.Extend(8);
        SetChunkHeader(wdlFile, curPos, 'MAHO', 2 * 16); // 1 hole mask for each chunk
        curPos += 8;

        wdlFile.Extend(32);
        curPos += 32;
    }
    delete[] mare_offsets;

    // Same replacement as the WDT above. This one is called from the map creation wizard
    // (MapCreationWizard.cpp:934) on a map that may already have a hand-authored low-detail mesh,
    // and truncate-then-write would destroy it on any failure. Bytes unchanged: wdlFile.all_data()
    // is exactly what setBuffer received.
    BlizzardArchive::Listfile::FileKey const file_key (filename.str());

    std::filesystem::path const target
      ( Noggit::Application::NoggitApplication::instance()->clientData()->getDiskPath (file_key) );

    Noggit::writeFileGuarded (target, wdlFile.all_data());
}

MapTileEntry::~MapTileEntry()
{
}

MapTileEntry::MapTileEntry()
  : flags(0)
  , tile(nullptr)
  , onDisc(false)
{
}
