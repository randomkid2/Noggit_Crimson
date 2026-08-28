// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/texturing/TextureLayerOps.hpp>

#include <noggit/Action.hpp>
#include <noggit/ActionManager.hpp>
#include <noggit/Log.h>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/MapView.h>
#include <noggit/TileIndex.hpp>
#include <noggit/World.h>
#include <noggit/World.inl>
#include <noggit/map_index.hpp>
#include <noggit/texture_set.hpp>

#include <exception>
#include <functional>
#include <utility>

namespace Noggit
{
  bool LayerOpResult::changedAnything() const
  {
    return chunks_changed != 0;
  }

  namespace
  {
    // Every chunk the scope resolves to, with the tile it belongs to.
    //
    // `visit` returns whether it changed the chunk, and that return value is what marks the tile
    // unsaved. The distinction matters: MapTile::changed is what map_index tests when deciding
    // which ADTs saveChanged writes (map_index.cpp), so marking a tile from a pass that turned out
    // to have nothing to do would make "Save all" rewrite files the user never edited. That is
    // also why the Chunk and Tile cases are walked by hand rather than through World::for_chunk_at
    // and World::for_tile_at, both of which call mapIndex.setChanged unconditionally on entry
    // (World.inl).
    void forEachChunkInScope
      ( MapView* map_view
      , LayerOpScopeRequest const& scope
      , std::function<bool(MapTile*, MapChunk*)> const& visit
      )
    {
      World* const world = map_view->getWorld();

      switch (scope.scope)
      {
        case LayerOpScope::Brush:
        {
          // The one case that can span four tiles at a corner, and World already owns the correct
          // walk for it -- tiles_in_range then chunks_in_range -- including the setChanged on a
          // true return.
          world->for_all_chunks_in_range
            ( scope.position
            , scope.radius
            , [&visit] (MapChunk* chunk)
              {
                return chunk && chunk->mt && visit(chunk->mt, chunk);
              }
            );
          break;
        }

        case LayerOpScope::Chunk:
        {
          MapChunk* const chunk = world->getChunkAt(scope.position);

          if (!chunk || !chunk->mt || !chunk->mt->finishedLoading())
          {
            return;
          }

          if (visit(chunk->mt, chunk))
          {
            world->mapIndex.setChanged(chunk->mt);
          }

          break;
        }

        case LayerOpScope::Tile:
        {
          MapTile* const tile = world->mapIndex.getTile(TileIndex(scope.position));

          // finishedLoading as well as non-null: getTile is a lookup that never triggers a load,
          // and reading a half-parsed tile's chunk array is a data race.
          if (!tile || !tile->finishedLoading())
          {
            return;
          }

          bool changed = false;

          for (unsigned int chunk_z = 0; chunk_z < 16; ++chunk_z)
          {
            for (unsigned int chunk_x = 0; chunk_x < 16; ++chunk_x)
            {
              if (MapChunk* const chunk = tile->getChunk(chunk_x, chunk_z))
              {
                changed |= visit(tile, chunk);
              }
            }
          }

          if (changed)
          {
            world->mapIndex.setChanged(tile);
          }

          break;
        }
      }
    }

    // ONE ACTION FOR THE WHOLE RUN, and that is a correctness requirement rather than a
    // convenience.
    //
    // Every one of these operations removes or rewrites layers across many chunks at once, and the
    // chunks are not independent of each other in the user's head: "purge the invisible layers on
    // this tile" is one decision, and a stack of 256 undo entries for it is not an undo, it is a
    // punishment. Action::registerChunkTextureChange stores one before-image per chunk on the
    // single running action and ignores a chunk it has already seen (Action.cpp), and
    // Action::finish then snapshots the after-image for every registered chunk, so one action
    // holds the whole batch in both directions and one Ctrl+Z reverts all of it.
    //
    // The try/catch is not decoration: leaving the manager holding a live action makes every later
    // beginAction quietly return that one instead of starting a new one (ActionManager.cpp:64), so
    // the next dozen brush strokes would all land in this batch's undo entry.
    LayerOpResult runInOneAction(MapView* map_view, std::function<void(LayerOpResult&)> const& body)
    {
      LayerOpResult result;

      if (!map_view || !map_view->getWorld())
      {
        result.error = "No world is loaded.";
        return result;
      }

      NOGGIT_ACTION_MGR->beginAction(map_view, Noggit::ActionFlags::eCHUNKS_TEXTURE);

      try
      {
        body(result);
      }
      catch (std::exception const& e)
      {
        NOGGIT_ACTION_MGR->endAction();
        LogError << "Texture layer operation failed: " << e.what() << std::endl;
        result.error = e.what();
        return result;
      }

      NOGGIT_ACTION_MGR->endAction();

      return result;
    }

    // Registers the chunk on the running action the first time this run touches it.
    //
    // Called BEFORE the first write and only for chunks that are really about to be written:
    // registerChunkTextureChange is idempotent per chunk, but snapshotting a chunk that turns out
    // to need nothing would copy up to three alpha planes and a 64 KiB float scratch buffer for a
    // no-op pair in the undo step.
    void registerBeforeWriting(MapChunk* chunk)
    {
      NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
    }
  }

  namespace TextureLayerOps
  {
    LayerOpResult purgeDuplicates(MapView* map_view, LayerOpScopeRequest const& scope)
    {
      return runInOneAction
        ( map_view
        , [&] (LayerOpResult& result)
          {
            forEachChunkInScope
              ( map_view
              , scope
              , [&result] (MapTile*, MapChunk* chunk) -> bool
                {
                  ++result.chunks_visited;

                  TextureSet* const texture_set = chunk->getTextureSet();

                  if (!texture_set || texture_set->num() < 2)
                  {
                    return false;
                  }

                  // Asked before registering, so a chunk with no duplicates costs at most six
                  // string compares -- four layers is six pairs -- instead of an alpha plane copy.
                  //
                  // Compared by stored filename while purgeDuplicateLayers compares the texture
                  // objects themselves. The two agree because the texture manager interns by file
                  // key, so two layers naming one path share one blp_texture; and where they could
                  // ever disagree this one is the more permissive, so the worst case is a chunk
                  // registered for a purge that then removes nothing, which is handled below.
                  bool has_duplicate = false;

                  for (std::size_t i = 0; i < texture_set->num() && !has_duplicate; ++i)
                  {
                    for (std::size_t j = i + 1; j < texture_set->num(); ++j)
                    {
                      if (texture_set->filename(i) == texture_set->filename(j))
                      {
                        has_duplicate = true;
                        break;
                      }
                    }
                  }

                  if (!has_duplicate)
                  {
                    return false;
                  }

                  registerBeforeWriting(chunk);

                  int const removed = texture_set->purgeDuplicateLayers();

                  if (removed <= 0)
                  {
                    return false;
                  }

                  result.layers_removed += static_cast<std::size_t>(removed);
                  ++result.chunks_changed;

                  return true;
                }
              );
          }
        );
    }

    LayerOpResult purgeBelowThreshold( MapView* map_view
                                     , LayerOpScopeRequest const& scope
                                     , std::uint8_t threshold
                                     )
    {
      return runInOneAction
        ( map_view
        , [&] (LayerOpResult& result)
          {
            forEachChunkInScope
              ( map_view
              , scope
              , [&result, threshold] (MapTile*, MapChunk* chunk) -> bool
                {
                  ++result.chunks_visited;

                  TextureSet* const texture_set = chunk->getTextureSet();

                  if (!texture_set || texture_set->num() < 2)
                  {
                    return false;
                  }

                  // The profile is the cheap half of the work -- one 4096-texel pass -- and it is
                  // what decides whether the expensive half happens at all, so it runs before the
                  // undo snapshot rather than inside purgeLayersBelowThreshold's own copy of it.
                  LayerAlphaProfile const profile = texture_set->layerAlphaProfile();

                  bool has_victim = false;

                  for (std::size_t layer = 0; layer < profile.layers; ++layer)
                  {
                    if (profile.peak[layer] <= threshold)
                    {
                      has_victim = true;
                      break;
                    }
                  }

                  if (!has_victim)
                  {
                    return false;
                  }

                  registerBeforeWriting(chunk);

                  int const removed = texture_set->purgeLayersBelowThreshold(threshold);

                  if (removed <= 0)
                  {
                    // Reachable and not an error: every layer was under the threshold, so the
                    // "never empty a chunk" rule kept the most visible one and there was nothing
                    // left to remove.
                    return false;
                  }

                  result.layers_removed += static_cast<std::size_t>(removed);
                  ++result.chunks_changed;

                  return true;
                }
              );
          }
        );
    }

    LayerOpResult replaceLayer( MapView* map_view
                              , LayerOpScopeRequest const& scope
                              , std::size_t slot
                              , std::string const& texture_path
                              , LayerAlphaHandling alpha_handling
                              )
    {
      LayerOpResult early;

      if (texture_path.empty())
      {
        early.error = "No texture chosen.";
        return early;
      }

      if (slot >= LayerAlphaProfile::MAX_LAYERS)
      {
        early.error = "Slot must be 0 to 3.";
        return early;
      }

      return runInOneAction
        ( map_view
        , [&] (LayerOpResult& result)
          {
            // Constructed once for the whole run rather than once per chunk: each construction is
            // a lookup in the async object map, and a tile scope is 256 chunks.
            scoped_blp_texture_reference const texture
              (texture_path, map_view->getRenderContext());

            forEachChunkInScope
              ( map_view
              , scope
              , [&] (MapTile*, MapChunk* chunk) -> bool
                {
                  ++result.chunks_visited;

                  TextureSet* const texture_set = chunk->getTextureSet();

                  if (!texture_set)
                  {
                    ++result.chunks_refused;
                    return false;
                  }

                  if (slot >= texture_set->num())
                  {
                    // The chunk has no such slot, and replaceLayerTexture will not fabricate the
                    // layers in between. Counted so the closing message can say how much of the
                    // area was skipped rather than silently doing less than it claimed.
                    ++result.chunks_refused;
                    return false;
                  }

                  registerBeforeWriting(chunk);

                  if (!texture_set->replaceLayerTexture(slot, texture, alpha_handling))
                  {
                    return false;
                  }

                  ++result.layers_replaced;
                  ++result.chunks_changed;

                  return true;
                }
              );
          }
        );
    }

    LayerOpResult prepareArea( MapView* map_view
                             , LayerOpScopeRequest const& scope
                             , PrepareAreaRequest const& request
                             )
    {
      LayerOpResult early;

      if (!request.clear_overlays && request.palette.empty())
      {
        early.error = "Nothing to do: tick Clear overlays or add a texture to the palette.";
        return early;
      }

      if (request.palette.size() > LayerAlphaProfile::MAX_LAYERS)
      {
        early.error = "A chunk holds four texture layers; the palette cannot be longer than four.";
        return early;
      }

      return runInOneAction
        ( map_view
        , [&] (LayerOpResult& result)
          {
            std::vector<scoped_blp_texture_reference> palette;
            palette.reserve(request.palette.size());

            for (std::string const& path : request.palette)
            {
              if (!path.empty())
              {
                palette.emplace_back(path, map_view->getRenderContext());
              }
            }

            // The admission policy this run uses is built here rather than read from
            // TextureLayerAdmission::current(), so that Prepare Area does exactly what its own
            // checkbox says regardless of what the brush's mode selector happens to be set to.
            TextureLayerAdmission admission;
            admission.policy = request.evict_to_fit
              ? LayerFullPolicy::ReplaceLeastVisible
              : LayerFullPolicy::Skip;

            forEachChunkInScope
              ( map_view
              , scope
              , [&] (MapTile*, MapChunk* chunk) -> bool
                {
                  ++result.chunks_visited;

                  TextureSet* const texture_set = chunk->getTextureSet();

                  if (!texture_set)
                  {
                    ++result.chunks_refused;
                    return false;
                  }

                  bool registered = false;
                  bool changed = false;

                  auto const ensure_registered = [&]
                  {
                    if (!registered)
                    {
                      registerBeforeWriting(chunk);
                      registered = true;
                    }
                  };

                  // CLEARING FIRST IS WHAT MAKES THE PALETTE FIT. A chunk holding four layers has
                  // no room for anything; once it is back to its base texture it has three free
                  // slots and the palette loop below never has to evict.
                  if (request.clear_overlays && texture_set->num() > 1)
                  {
                    ensure_registered();

                    int const removed = texture_set->clearOverlayLayers();

                    if (removed > 0)
                    {
                      result.layers_removed += static_cast<std::size_t>(removed);
                      changed = true;
                    }
                  }

                  bool refused_any = false;

                  for (scoped_blp_texture_reference const& texture : palette)
                  {
                    if (texture_set->texture_id(texture) != -1)
                    {
                      continue;
                    }

                    // Asked before registering so a chunk that cannot take anything is not
                    // snapshotted for nothing.
                    if (!texture_set->canAdmitTexture(texture, admission))
                    {
                      refused_any = true;
                      continue;
                    }

                    ensure_registered();

                    // A target of 1 rather than 0: get_texture_index_or_add returns -1 outright
                    // for a target of 0 ("don't add a texture for nothing"), which is the right
                    // rule for a brush painting transparency and the wrong one for a pass whose
                    // whole purpose is to reserve the slot. The layer arrives with zero alpha --
                    // addTexture builds the plane with Alphamap's default constructor, which
                    // memsets 4096 bytes to 0 -- so nothing changes on screen until it is painted.
                    //
                    // HOW LONG A RESERVATION LASTS, stated plainly because the honest answer is
                    // "until the next stroke on that chunk". paintTexture ends with a call to
                    // eraseUnusedTextures, which drops every layer that is still invisible, and a
                    // reserved-but-unpainted layer is invisible by definition. So the palette pass
                    // is a CHECK plus a temporary reservation, not a permanent one: it proves the
                    // palette fits and tells you through chunks_refused exactly where it does not,
                    // and the layer the user then paints stays because painting makes it visible.
                    // A user who wants reservations to survive can turn the cleanup off -- it is
                    // gated on the "cleanup_unused_textures" QSetting, which eraseUnusedTextures
                    // reads on every call. Clearing the overlays, by contrast, is permanent: a
                    // removed layer does not come back.
                    int const layer = texture_set->get_texture_index_or_add(texture, 1.f, admission);

                    if (layer < 0)
                    {
                      refused_any = true;
                      continue;
                    }

                    ++result.layers_added;
                    changed = true;
                  }

                  if (refused_any)
                  {
                    ++result.chunks_refused;
                  }

                  if (changed)
                  {
                    ++result.chunks_changed;
                  }

                  return changed;
                }
              );
          }
        );
    }
  }
}
