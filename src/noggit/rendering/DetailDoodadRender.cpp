// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/rendering/DetailDoodadRender.hpp>

#include <noggit/DBC.h>
#include <noggit/MapChunk.h>
#include <noggit/MapHeaders.h>
#include <noggit/MapTile.h>
#include <noggit/Model.h>
#include <noggit/database/ModelPathFixup.hpp>
#include <noggit/rendering/ModelRender.hpp>
#include <noggit/texture_set.hpp>

#include <math/frustum.hpp>
#include <util/CurrentFunction.hpp>

#include <Listfile.hpp>
#include <external/tracy/Tracy.hpp>

#include <algorithm>
#include <array>
#include <iterator>
#include <string>
#include <utility>

namespace
{
  // Half the diagonal of a chunk's footprint, used to grow the per-chunk distance test so a chunk
  // whose centre is just outside the draw distance still contributes the doodads near its edge.
  // sqrt(2) / 2 * CHUNKSIZE == 0.70711 * 33.333 == 23.57.
  float const CHUNK_BOUND_RADIUS = 0.70710678f * CHUNKSIZE;

  // Half a tile's diagonal, the same idea one level up. 0.70711 * 533.333 == 377.1.
  float const TILE_BOUND_RADIUS = 0.70710678f * TILESIZE;

  // Where GroundEffectDoodad.Filename is rooted. The DBC stores a bare file name; the client
  // prefixes this. Same directory the Ground Effects tool already builds its preview thumbnails
  // from (GroundEffectsTool.cpp:1381), so a doodad that renders in the tool renders here.
  constexpr char const* DETAIL_DOODAD_ROOT = "world/nodxt/detail/";

  std::uint64_t fnv1a(std::uint64_t hash, void const* data, std::size_t size)
  {
    auto const* bytes = static_cast<std::uint8_t const*>(data);

    for (std::size_t i = 0; i < size; ++i)
    {
      hash ^= bytes[i];
      hash *= 0x00000100000001B3ull;
    }

    return hash;
  }
}

namespace Noggit::Rendering
{
  DetailDoodadRender::DetailDoodadRender(Noggit::NoggitRenderContext context)
    : _context(context)
  {
  }

  bool DetailDoodadRender::enabled() const
  {
    return _enabled;
  }

  void DetailDoodadRender::setEnabled(bool enabled)
  {
    if (_enabled == enabled)
    {
      return;
    }

    _enabled = enabled;

    // The cache is deliberately KEPT when the preview is switched off. Toggling it back on is the
    // single most common thing a user does with this control -- look, hide, look again -- and
    // rebuilding forty chunks for each of those is a stutter for nothing. The stale-entry sweep
    // reclaims it after ten seconds if the user really is done.
  }

  float DetailDoodadRender::drawDistance() const
  {
    return _draw_distance;
  }

  void DetailDoodadRender::setDrawDistance(float distance)
  {
    _draw_distance = std::max(0.0f, distance);

    // No invalidation. Draw distance decides which cached chunks are visited, never what is in
    // them, so raising it only brings already-correct entries back into view.
  }

  unsigned DetailDoodadRender::density() const
  {
    return _density;
  }

  void DetailDoodadRender::setDensity(unsigned density)
  {
    density = std::clamp(density, DETAIL_DOODAD_MIN_DENSITY, DETAIL_DOODAD_MAX_DENSITY);

    if (_density == density)
    {
      return;
    }

    _density = density;

    // Unlike draw distance, this one does invalidate. Density is the number of draws taken from
    // the generator, so it changes the random stream rather than scaling its output: every doodad
    // in the chunk moves, not just the ones past the old count. The per-chunk `density` field
    // causes the rebuild lazily, as each chunk next comes into view, rather than all at once here.
  }

  void DetailDoodadRender::clear()
  {
    _chunk_cache.clear();
    _batches.clear();

    // Last, and the reason this function needs a bound context: releasing the final reference to a
    // detail doodad model destroys its OpenGL vertex arrays.
    _models.clear();

    _instances_last_frame = 0;
    _chunks_last_frame = 0;
    _rebuilds_last_frame = 0;
  }

  std::uint32_t DetailDoodadRender::chunkIdentity(MapChunk* chunk)
  {
    // The chunk's global position on the map: tile index times sixteen plus the chunk's own index,
    // packed z in the high half and x in the low half. Same quantity the placement seeds from, and
    // it is unique across the whole map.
    return (static_cast<std::uint32_t>(chunk->mt->index.z * 16u + chunk->py) << 16u)
         | static_cast<std::uint32_t>(chunk->mt->index.x * 16u + chunk->px);
  }

  std::uint64_t DetailDoodadRender::chunkSignature(MapChunk* chunk)
  {
    std::uint64_t hash = 0xCBF29CE484222325ull;

    std::uint32_t const identity = chunkIdentity(chunk);
    hash = fnv1a(hash, &identity, sizeof(identity));

    TextureSet* const texture_set = chunk->texture_set.get();

    if (!texture_set)
    {
      return hash;
    }

    // 16 bytes: two bits per cell naming which layer owns that cell's doodads.
    std::array<std::uint16_t, 8> const& mapping = texture_set->getDoodadMapping();
    hash = fnv1a(hash, mapping.data(), mapping.size() * sizeof(std::uint16_t));

    // 8 bytes: THE EXCLUSION MAP, one bit per cell. Hashed so that a stroke of the exclusion brush
    // rebuilds the chunk on the very next frame it is visible.
    hash = fnv1a(hash, texture_set->_doodadStencil.data(), texture_set->_doodadStencil.size());

    // 16 bytes: the four layers' ground effect ids, which is what the effect brush edits.
    for (std::size_t layer = 0; layer < 4; ++layer)
    {
      unsigned const effect_id = texture_set->getEffectForLayer(layer);
      hash = fnv1a(hash, &effect_id, sizeof(effect_id));
    }

    // 4 bytes. Redundant with the surface revision, which registerChunkUpdate also bumps for
    // HOLES, and kept anyway: it costs four byte-multiplies and it means a hole edit that somehow
    // reached the chunk without going through registerChunkUpdate still invalidates.
    int const holes = chunk->holes;
    hash = fnv1a(hash, &holes, sizeof(holes));

    return hash;
  }

  Model* DetailDoodadRender::resolveModel(std::uint32_t doodad_id)
  {
    auto entry (_models.find(doodad_id));

    if (entry == _models.end())
    {
      DoodadModel resolved;

      try
      {
        char const* const dbc_name
          (gGroundEffectDoodadDB.getByID(doodad_id).getString(GroundEffectDoodadDB::Filename));

        if (dbc_name && *dbc_name)
        {
          // Through the project's anchored, case-insensitive rewriter rather than the
          // QString::replace the Ground Effects tool open-codes. GroundEffectDoodad names are
          // .mdx in the DBC and .m2 in the archive, and the tool's version replaces the extension
          // wherever it appears in the path rather than only at the end -- see ModelPathFixup.hpp,
          // which names that call site as one of the wrong ones.
          std::string const path
            (Noggit::Database::ModelPathFixup::toM2Path(DETAIL_DOODAD_ROOT + std::string(dbc_name)));

          if (!path.empty())
          {
            resolved.reference.emplace(BlizzardArchive::Listfile::FileKey(path), _context);
          }
        }
      }
      catch (DBCFile::NotFound const&)
      {
        // A GroundEffectTexture row pointing at a doodad row that does not exist. Cached as an
        // empty entry below, so this costs one failed lookup for the session rather than one per
        // chunk per rebuild.
      }

      entry = _models.emplace(doodad_id, std::move(resolved)).first;
    }

    if (!entry->second.reference)
    {
      return nullptr;
    }

    Model* const model = entry->second.reference->get();

    // Still streaming in, or gone for good. Either way there is nothing to draw this frame; the
    // reference is held, so a model that is still loading will be there on a later one.
    if (!model || !model->finishedLoading() || model->loading_failed())
    {
      return nullptr;
    }

    // A model the user has hidden is skipped outright rather than drawn as a bounding box. Boxes
    // around ten thousand tufts of grass would be unreadable, and letting ModelRender record a box
    // count here would collide with the count the database spawn pass records for the same model,
    // which is the hazard documented at WorldRender.cpp's db_spawns_to_draw loop.
    if (model->is_hidden())
    {
      return nullptr;
    }

    return model;
  }

  void DetailDoodadRender::beginFrame()
  {
    ++_frame;

    // Carried over here rather than at the end of draw(), so the read-out stays truthful on a
    // frame where draw() returns early because the preview was switched off mid-frame.
    _rebuilds_last_frame = _rebuilds_this_frame;

    _rebuilds_this_frame = 0;
    _instances_last_frame = 0;
    _chunks_last_frame = 0;

    // clear() on the vectors, not on the map: the per-model transform buffers keep their capacity,
    // so a view that is not changing allocates nothing after its first frame.
    for (auto& batch : _batches)
    {
      batch.second.clear();
    }

    if (_frame % SWEEP_INTERVAL_FRAMES == 0)
    {
      for (auto it = _chunk_cache.begin(); it != _chunk_cache.end();)
      {
        it = (_frame - it->second.last_used_frame > CACHE_LIFETIME_FRAMES)
           ? _chunk_cache.erase(it)
           : std::next(it);
      }
    }
  }

  void DetailDoodadRender::gatherTile(MapTile* tile, glm::vec3 const& camera_pos
                                     , math::frustum const& frustum)
  {
    // finishedLoading() rather than the `finished` atomic TileRender reads directly: that member
    // is protected on AsyncObject and TileRender only reaches it as a friend of MapTile.
    if (!_enabled || !tile || !tile->finishedLoading())
    {
      return;
    }

    ZoneScopedN(NOGGIT_CURRENT_FUNCTION);

    // One test for 256 chunks. The doodad draw distance is much shorter than the terrain view
    // distance, so on a normal frame most loaded tiles leave here immediately.
    if (tile->camDist() > _draw_distance + TILE_BOUND_RADIUS)
    {
      return;
    }

    for (unsigned z = 0; z < 16; ++z)
    {
      for (unsigned x = 0; x < 16; ++x)
      {
        MapChunk* const chunk = tile->getChunk(x, z);

        if (!chunk || !chunk->texture_set)
        {
          continue;
        }

        if (glm::distance(camera_pos, chunk->vcenter) > _draw_distance + CHUNK_BOUND_RADIUS)
        {
          continue;
        }

        if (!frustum.intersects(chunk->vmax, chunk->vmin))
        {
          continue;
        }

        ChunkCache& cache = _chunk_cache[chunk];

        std::uint64_t const signature = chunkSignature(chunk);
        std::uint32_t const revision = chunk->detailDoodadSurfaceRevision();

        bool const stale = cache.last_used_frame == 0
                        || cache.signature != signature
                        || cache.surface_revision != revision
                        || cache.density != _density;

        if (stale)
        {
          // Budgeted. A chunk that misses the budget keeps last frame's placements for one more
          // frame and is rebuilt on the next -- nothing here clears the mismatch, so the candidacy
          // survives. Drawing slightly stale grass for a frame or two after a stroke is a far
          // better trade than the hitch of rebuilding a whole stroke's worth at once.
          if (_rebuilds_this_frame < MAX_REBUILDS_PER_FRAME)
          {
            cache.groups = buildChunkDetailDoodads(chunk, _density);
            cache.signature = signature;
            cache.surface_revision = revision;
            cache.density = _density;

            ++_rebuilds_this_frame;
          }
          else if (cache.last_used_frame == 0)
          {
            // Never built at all. Drawing nothing is right; leave the entry marked as used so the
            // sweep does not drop it before it gets its turn.
            cache.last_used_frame = _frame;
            continue;
          }
        }

        cache.last_used_frame = _frame;
        ++_chunks_last_frame;

        for (DetailDoodadGroup const& group : cache.groups)
        {
          Model* const model = resolveModel(group.doodad_id);

          if (!model)
          {
            continue;
          }

          std::vector<glm::mat4x4>& batch = _batches[model];
          batch.insert(batch.end(), group.transforms.begin(), group.transforms.end());
        }
      }
    }
  }

  void DetailDoodadRender::draw( glm::mat4x4 const& model_view
                               , OpenGL::Scoped::use_program& m2_shader
                               , OpenGL::M2RenderState& model_render_state
                               , math::frustum const& frustum
                               , float cull_distance
                               , glm::vec3 const& camera_pos
                               , int animtime
                               , std::unordered_map<Model*, std::size_t>& model_boxes_to_draw
                               , display_mode display
                               )
  {
    if (!_enabled)
    {
      return;
    }

    ZoneScopedN(NOGGIT_CURRENT_FUNCTION);

    for (auto& batch : _batches)
    {
      if (batch.second.empty())
      {
        continue;
      }

      // all_boxes false, animate false, both box flags false. Grass is never outlined, and it has
      // no animation worth the per-frame bone evaluation -- a hidden model never reaches here at
      // all, so nothing in this call can record a box count that the shared per-model transform
      // buffer would then contradict.
      batch.first->renderer()->draw( model_view
          , batch.second
          , m2_shader
          , model_render_state
          , frustum
          , cull_distance
          , camera_pos
          , animtime
          , false
          , model_boxes_to_draw
          , display
          , false
          , false
          , false
          , false
      );

      _instances_last_frame += batch.second.size();
    }
  }

  std::size_t DetailDoodadRender::lastFrameInstanceCount() const
  {
    return _instances_last_frame;
  }

  std::size_t DetailDoodadRender::lastFrameChunkCount() const
  {
    return _chunks_last_frame;
  }

  std::size_t DetailDoodadRender::lastFrameRebuildCount() const
  {
    return _rebuilds_last_frame;
  }

  std::size_t DetailDoodadRender::cachedChunkCount() const
  {
    return _chunk_cache.size();
  }
}
