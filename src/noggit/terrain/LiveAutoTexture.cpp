// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/terrain/LiveAutoTexture.hpp>
#include <noggit/terrain/TerrainRuleStore.hpp>

#include <noggit/Action.hpp>
#include <noggit/ActionManager.hpp>
#include <noggit/Log.h>
#include <noggit/MapChunk.h>
#include <noggit/MapHeaders.h>
#include <noggit/MapTile.h>
#include <noggit/MapView.h>
#include <noggit/World.h>
#include <opengl/context.hpp>
#include <opengl/scoped.hpp>

#include <glm/vec3.hpp>

#include <cstring>
#include <exception>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
  // Action stores a chunk's pre-edit vertices as a flat std::array<float, 145 * 3> and memcpy's it
  // straight out of the glm::vec3 array (Action.cpp:692), so the two layouts have to agree. They
  // do -- glm::vec3 is three floats with no padding -- but the dependency is silent, and this pass
  // memcmp's the same two buffers against each other, so it is worth making the compiler check it.
  static_assert( sizeof(glm::vec3) == 3 * sizeof(float)
               , "MapChunk::mVertices is compared against Action's flat float snapshot byte for "
                 "byte; a padded glm::vec3 would make that comparison meaningless."
               );

  constexpr std::size_t CHUNK_VERTEX_BYTES
    = static_cast<std::size_t>(Noggit::TerrainRuleCollector::CHUNK_VERTEX_COUNT) * 3 * sizeof(float);

  // Chunks the stroke really moved, out of the chunks it registered.
  //
  // These are NOT the same set, and the difference is not hypothetical. Registration happens before
  // an edit, on every chunk the tool intends to visit, and a tool is allowed to intend more than it
  // achieves: the erosion brush deliberately registers a margin of one lattice cell beyond its
  // radius so that both copies of a shared seam vertex move together (ErosionToolSettings.cpp:277),
  // and it says in the same comment that chunks gaining nothing return moved == 0. A raise/lower
  // stroke registers every chunk whose SQUARE is within the radius, which includes corners the
  // circular brush never reaches. Taking the registered list at face value would repaint terrain
  // the user did not touch -- over hand-painted alpha -- for no visible reason.
  //
  // Action::getChunkTerrainOriginalData hands back the pre-edit vertices it already cached for the
  // undo step, so proving movement costs one 1740-byte memcmp per chunk and no extra storage. A
  // chunk with no cached image cannot be proved to have moved and is dropped, which fails in the
  // safe direction: the worst outcome is that a chunk is not repainted.
  std::vector<MapChunk*> movedChunks(Noggit::Action* action, std::vector<MapChunk*> const& registered)
  {
    std::vector<MapChunk*> moved;
    moved.reserve(registered.size());

    for (MapChunk* chunk : registered)
    {
      if (!chunk || !chunk->mt)
      {
        continue;
      }

      float const* const original = action->getChunkTerrainOriginalData(chunk);

      if (original && std::memcmp(original, &chunk->mVertices, CHUNK_VERTEX_BYTES) != 0)
      {
        moved.push_back(chunk);
      }
    }

    return moved;
  }

  // The chunks a live pass will repaint: every chunk the stroke moved, plus the eight chunks
  // around each of them.
  //
  // THE SEED SET is movedChunks() above -- the exact footprint of the stroke. Not the tile:
  // retexturing a whole ADT because the user nudged one hillside would take a second and would
  // overwrite 255 chunks of hand-painted work the stroke never came near.
  //
  // THE RING is what stops the result ending in a straight line on a chunk boundary.
  // TextureSet::paintTexture clips to its own chunk -- it walks that chunk's 64x64 alphamap and
  // nothing else (texture_set.cpp:847) -- so a seed chunk repainted to rock beside an untouched
  // chunk still carrying grass puts the rock/grass transition exactly on the chunk grid, which is
  // a perfectly straight 33.3-unit edge and reads as a bug. Repainting the neighbours re-decides
  // their units against the same rules, and since the units next to the boundary have nearly the
  // slope of the seed chunk's units next to the same boundary, they resolve to the same rule. The
  // transition then falls where the SLOPE changes, which is where the user expects it.
  //
  // Diagonals are included. A four-neighbourhood leaves the four corner chunks unrepainted, which
  // is the same straight-edge artefact reduced to a notch rather than removed.
  //
  // AT A TILE BOUNDARY the neighbour may not be loaded, and then it is skipped. World::getChunkAt
  // is a lookup that returns null for an unloaded or still-parsing tile (World.cpp:1117-1125) and
  // never triggers a load, which is the behaviour wanted here twice over: reading a half-parsed
  // tile's chunk array is a data race, and pulling an ADT into memory as a side effect of a brush
  // stroke would mark a file the user never opened as unsaved. The visible cost is the artefact
  // above, on that one tile edge, until the neighbouring tile is loaded and touched.
  //
  // A seed chunk with a null tile pointer is dropped rather than trusted; nothing in the tree
  // produces one, but the whole set is about to be handed to a painter that will mark tiles
  // changed.
  std::vector<MapChunk*> gatherStrokeChunks(World* world, std::vector<MapChunk*> const& seeds)
  {
    std::vector<MapChunk*> gathered;
    std::unordered_set<MapChunk*> seen;

    gathered.reserve(seeds.size() * 9);
    seen.reserve(seeds.size() * 9);

    auto const add = [&] (MapChunk* chunk)
    {
      if (chunk && chunk->mt && seen.insert(chunk).second)
      {
        gathered.push_back(chunk);
      }
    };

    for (MapChunk* seed : seeds)
    {
      add(seed);
    }

    // Iterated over `seeds` rather than over `gathered`, which is growing: a ring around the ring
    // would be two chunks of bleed on every side of the stroke, and at that point the pass is
    // repainting terrain the user cannot see a reason for.
    for (MapChunk* seed : seeds)
    {
      if (!seed)
      {
        continue;
      }

      for (int dz = -1; dz <= 1; ++dz)
      {
        for (int dx = -1; dx <= 1; ++dx)
        {
          if (dx == 0 && dz == 0)
          {
            continue;
          }

          // Probed at the neighbour's CENTRE, in world coordinates, rather than by stepping px/py
          // inside the tile. The centre probe crosses a tile boundary for free and falls off the
          // 64x64 tile grid safely at the edge of the map: TileIndex floors a negative coordinate
          // into a std::size_t, which wraps to a value is_valid() rejects (TileIndex.cpp:32-36),
          // and MapIndex::getTile then returns null.
          glm::vec3 const probe
            ( seed->xbase + CHUNKSIZE * (0.5f + static_cast<float>(dx))
            , seed->ybase
            , seed->zbase + CHUNKSIZE * (0.5f + static_cast<float>(dz))
            );

          add(world->getChunkAt(probe));
        }
      }
    }

    return gathered;
  }

  std::size_t runAgainstOpenAction(MapView* map_view)
  {
    Noggit::TerrainRuleStore* const store = Noggit::TerrainRuleStore::instance();

    // Asked first and asked cheaply. This runs on every frame that ends a stroke of any kind, and
    // for the overwhelming majority of users the answer is "the switch is off" before anything is
    // allocated or any rule is built.
    if (!store->liveAutoRunnable())
    {
      return 0;
    }

    if (!map_view)
    {
      return 0;
    }

    Noggit::Action* const action = NOGGIT_ACTION_MGR->getCurrentAction();

    if (!action)
    {
      return 0;
    }

    // eCHUNKS_TERRAIN rather than "some action is open": the same modality mismatch closes object
    // drags, vertex selections and the radius/speed changes the terrain tools open on the right
    // mouse button (RaiseLowerTool.cpp:172-217), none of which move a vertex. Those actions carry
    // no registered terrain chunks either, so the next check would catch them anyway -- this one is
    // here so the common case costs a flag test rather than a vector copy.
    if (!(action->getFlags() & Noggit::ActionFlags::eCHUNKS_TERRAIN))
    {
      return 0;
    }

    // A STROKE THAT PAINTED ITS OWN TEXTURES IS LEFT ALONE, and this is not a theoretical case.
    // The Brush Stack runs several brushes off one action -- BrushStackItem::execute switches on
    // the item type and a stack may hold a Raise/Lower item and a Texturing item together
    // (BrushStackItem.cpp:437-470) -- so one shift-drag can raise a ridge AND paint snow on it,
    // both inside the eSHIFT|eLMB action this hook is about to fire on. The terrain half sets
    // eCHUNKS_TERRAIN, so the test above passes, and the pass would then repaint the ridge from the
    // rules and wipe the snow the user's own brush had just laid down in the same gesture. It is
    // recoverable -- everything is in one action, so one Ctrl+Z takes back the shape, the snow and
    // the overpaint together -- but it silently defeats the stack, and a stacked texturing brush is
    // a much more specific instruction than a rule set is.
    //
    // eCHUNKS_TEXTURE is the exact discriminator: sculpting alone never sets it (the terrain paths
    // in World.cpp call registerChunkTerrainChange only), and the flag is raised by
    // registerChunkTextureChange the moment any texture brush touches a chunk (Action.cpp:699).
    if (action->getFlags() & Noggit::ActionFlags::eCHUNKS_TEXTURE)
    {
      return 0;
    }

    std::vector<MapChunk*> const seeds (movedChunks(action, action->terrainChangedChunks()));

    if (seeds.empty())
    {
      return 0;
    }

    World* world = map_view->getWorld();

    if (!world)
    {
      return 0;
    }

    std::vector<MapChunk*> const chunks (gatherStrokeChunks(world, seeds));

    if (chunks.empty())
    {
      return 0;
    }

    // Adding a texture layer constructs a scoped_blp_texture_reference, which loads and uploads a
    // BLP. Action::undo makes the context current before doing the same thing (Action.cpp:55-56);
    // this path has the same requirement. MapView::tick is on the GUI thread and the context is
    // normally current already, but "normally" is not a guarantee and the cost of asking is a
    // no-op when it already is.
    map_view->context()->makeCurrent(map_view->context()->surface());
    OpenGL::context::scoped_setter const context_setter (::gl, map_view->context());

    Noggit::TerrainRulePainter painter (map_view, store->ruleSet());

    std::string error;

    if (!painter.prepareTextures(error))
    {
      // Logged once per stroke and otherwise silent. A message box here would appear in the middle
      // of sculpting, on every stroke, for as long as the texture stayed missing -- and the user
      // has a dialog to go and look at. Standing down leaves the terrain edit itself untouched and
      // fully undoable.
      LogError << "Live auto texture: a texture named by the rules could not be loaded, so the "
                  "stroke was left unpainted: " << error << std::endl;
      return 0;
    }

    // NO beginAction and NO endAction. The stroke's action is open right now and the painter
    // registers on it; see property 3 in LiveAutoTexture.hpp.
    //
    // The try is not decoration. This is reached from MapView::tick, which Qt calls as an event
    // handler, and an exception escaping one of those unwinds through Qt's own frames -- so a
    // texture that fails to load halfway through a stroke would take the editor down rather than
    // spoil one paint. Caught here, the chunks already painted stay painted, the stroke's action is
    // still open and is closed by the caller two lines later, and one undo reverts the lot.
    try
    {
      for (MapChunk* chunk : chunks)
      {
        painter.paintChunk(chunk->mt, chunk);
      }
    }
    catch (std::exception const& e)
    {
      LogError << "Live auto texture: the pass failed partway through and was stopped. The terrain "
                  "edit and whatever it had already painted are both in the current undo step: "
               << e.what() << std::endl;
    }

    return painter.stats().chunks_painted;
  }
}

namespace Noggit
{
  namespace LiveAutoTexture
  {
    std::size_t runIfStrokeEnding(MapView* map_view, unsigned action_modality)
    {
      // The cheapest question first, and the ordering is deliberate: this is called from
      // MapView::tick, so it runs on every frame the editor draws, and for a session where nobody
      // ticked the box that has to cost one bool read and nothing else. The expensive gate --
      // liveAutoRunnable, which rebuilds the rule set to validate it -- is inside
      // runAgainstOpenAction and is reached only on the one frame a stroke actually ends.
      if (!TerrainRuleStore::instance()->liveAutoEnabled())
      {
        return 0;
      }

      Action* const action = NOGGIT_ACTION_MGR->getCurrentAction();

      if (!action)
      {
        return 0;
      }

      // The predicate below is ActionManager::endActionOnModalityMismatch's own test
      // (ActionManager.cpp:139-147), restated. It is restated rather than shared because the
      // manager's version has a side effect -- it finishes the action -- and what is needed here is
      // the question without the answer. The two must agree: if this said "ending" when the manager
      // did not, the live pass would run mid-stroke on every frame, which is the exact failure mode
      // this hook exists to avoid; if it said "not ending" when the manager did, the paint would
      // simply never happen.
      //
      // An action opened with no modality controllers at all is never closed this way, so it is not
      // a stroke and is not ours. Those are the one-shot edits, and they go through runNow.
      if (!action->getModalityControllers())
      {
        return 0;
      }

      unsigned const controllers = static_cast<unsigned>(action->getModalityControllers());

      if ((action_modality & controllers) == controllers)
      {
        return 0;
      }

      return runAgainstOpenAction(map_view);
    }

    std::size_t runNow(MapView* map_view)
    {
      return runAgainstOpenAction(map_view);
    }
  }
}
