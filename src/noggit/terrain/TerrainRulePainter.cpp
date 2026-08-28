// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/terrain/TerrainRulePainter.hpp>

#include <noggit/Action.hpp>
#include <noggit/ActionManager.hpp>
#include <noggit/Log.h>
#include <noggit/MapChunk.h>
#include <noggit/MapHeaders.h>
#include <noggit/MapTile.h>
#include <noggit/MapView.h>
#include <noggit/World.h>
#include <noggit/map_index.hpp>
#include <noggit/texture_set.hpp>

#include <glm/vec3.hpp>

#include <algorithm>
#include <cstring>
#include <exception>
#include <string_view>
#include <utility>

namespace
{
  namespace Collector = Noggit::TerrainRuleCollector;

  constexpr int UNITS_PER_CHUNK = Collector::CHUNK_INNER_SIDE * Collector::CHUNK_INNER_SIDE;

  // Hardness just short of 1, and the reason is arithmetic rather than aesthetic: Brush::getValue
  // computes `1 - (dist - iradius) / oradius` (Brush.cpp:43), and hardness 1 makes oradius exactly
  // 0, so a texel landing precisely on the radius divides zero by zero and writes NaN into an
  // alpha map. At 0.98 the falloff band is 2% of the radius -- under a tenth of a texel at the
  // sizes used here -- so every texel that is touched is still touched at full strength.
  constexpr float BRUSH_HARDNESS = 0.98f;

  // Radius of the per-unit brush, in world units.
  //
  // A unit is 8x8 alphamap texels, so half a unit across is UNITSIZE/2. That is NOT enough: the
  // brush measures the shortest distance to each texel's SQUARE, and the corner texel of a unit
  // sits 3*TEXDETAILSIZE*sqrt(2) = 0.53*UNITSIZE from the unit centre, so a radius of half a unit
  // leaves the four corners of every unit unpainted -- speckle, in whatever was there before.
  // Rounded up to 0.56 to clear that with margin.
  //
  // The cost is about one texel of bleed into the neighbouring unit on each side, which no
  // circular brush can avoid and which does not matter: neighbours that agree on a texture
  // overwrite each other with the same value, and at a boundary the seam moves by half a metre.
  constexpr float UNIT_BRUSH_RADIUS = UNITSIZE * 0.56f;

  // Covers every texel of one chunk with room to spare -- the corner texel is 22.8 units from the
  // centre against a chunk 33.3 across -- and, because the paint is issued on the chunk directly
  // rather than through World::paintTexture, reaches nothing outside it.
  constexpr float CHUNK_BRUSH_RADIUS = CHUNKSIZE;

  // World position of the inner vertex of one unit.
  //
  // The inner vertex IS the unit centre, so this is a lookup rather than a derivation -- and it is
  // the same vertex TerrainRuleCollector::sampleChunkUnit read the slope from, which is what keeps
  // the decision and the paint at the same place.
  glm::vec3 unitCentre(MapChunk* chunk, int unit_row, int unit_col)
  {
    return chunk->mVertices[Collector::chunkInnerVertexIndex(unit_row, unit_col)];
  }
}

namespace Noggit
{
  void TerrainPaintStats::merge(TerrainPaintStats const& other)
  {
    chunks_painted += other.chunks_painted;
    chunks_uniform += other.chunks_uniform;
    chunks_unchanged += other.chunks_unchanged;
    units_painted += other.units_painted;
    units_refused += other.units_refused;
    units_unchanged += other.units_unchanged;
  }

  bool TerrainRulePainter::ChunkTextureFingerprint::operator==
    (ChunkTextureFingerprint const& other) const
  {
    return layers == other.layers
        && alpha_present == other.alpha_present
        && alpha == other.alpha;
  }

  void TerrainRulePainter::captureFingerprint(TextureSet* texture_set, ChunkTextureFingerprint& out)
  {
    out.layers.clear();

    for (std::size_t layer = 0; layer < texture_set->num(); ++layer)
    {
      out.layers.push_back(texture_set->filename(layer));
    }

    auto const& maps = *texture_set->getAlphamaps();

    for (std::size_t i = 0; i < static_cast<std::size_t>(MAX_ALPHAMAPS); ++i)
    {
      out.alpha_present[i] = maps[i] != nullptr;

      if (maps[i])
      {
        std::memcpy(out.alpha[i].data(), maps[i]->getAlpha(), out.alpha[i].size());
      }
      else
      {
        out.alpha[i].fill(0);
      }
    }
  }

  TerrainRulePainter::TerrainRulePainter(MapView* map_view, TerrainRuleSet rules)
    : _map_view(map_view)
    , _world(map_view ? map_view->getWorld() : nullptr)
    , _rules(std::move(rules))
  {
    _unit_brush.init();
    _unit_brush.setHardness(BRUSH_HARDNESS);
    _unit_brush.setRadius(UNIT_BRUSH_RADIUS);

    _chunk_brush.init();
    _chunk_brush.setHardness(BRUSH_HARDNESS);
    _chunk_brush.setRadius(CHUNK_BRUSH_RADIUS);
  }

  bool TerrainRulePainter::prepareTextures(std::string& error)
  {
    error.clear();

    if (!_map_view || !_world)
    {
      error = "no map view";
      return false;
    }

    try
    {
      // One reference per distinct texture rather than one per paint call: a tile-sized scope is
      // tens of thousands of calls and each construction is a lookup in the async object map.
      for (std::string const& path : _rules.distinctTextures())
      {
        if (path.empty())
        {
          continue;
        }

        _texture_refs.emplace
          (path, scoped_blp_texture_reference(path, _map_view->getRenderContext()));
      }
    }
    catch (std::exception const& e)
    {
      error = e.what();
      _texture_refs.clear();
      return false;
    }

    _textures_ready = true;
    return true;
  }

  TerrainPaintStats const& TerrainRulePainter::stats() const
  {
    return _stats;
  }

  TerrainRuleSet const& TerrainRulePainter::rules() const
  {
    return _rules;
  }

  void TerrainRulePainter::paintChunk(MapTile* tile, MapChunk* chunk)
  {
    if (!_textures_ready || !tile || !chunk)
    {
      return;
    }

    TextureSet* texture_set = chunk->texture_set.get();

    if (!texture_set)
    {
      return;
    }

    // Guarded rather than assumed. The dialog opens its action before the walk, but a caller that
    // reached this from a stroke hook is relying on someone else's action still being open, and
    // registering a before-image on a null action is a crash instead of a refusal.
    Action* const action = NOGGIT_ACTION_MGR->getCurrentAction();

    if (!action)
    {
      return;
    }

    std::array<TerrainRuleResult, UNITS_PER_CHUNK> decisions{};

    Collector::collectChunkUnits
      ( chunk
      , _rules
      , [&decisions] ( MapChunk*
                     , int unit_row
                     , int unit_col
                     , TerrainSample const&
                     , TerrainRuleResult const& unit_result
                     )
        {
          decisions[unit_row * Collector::CHUNK_INNER_SIDE + unit_col] = unit_result;
        }
      );

    // A chunk every rule agrees on is one brush pass instead of 64. Flat ground is most of most
    // maps, so this is the difference between a tile taking a moment and taking most of a second,
    // and it costs one comparison per unit to find out.
    bool uniform = decisions[0].matched;

    for (auto const& decision : decisions)
    {
      if ( !decision.matched
        || decision.texture != decisions[0].texture
        || decision.alpha != decisions[0].alpha
         )
      {
        uniform = false;
        break;
      }
    }

    // Registered on the first operation that will really touch the chunk -- a paint, or the layer
    // eviction resolve() may have to do to make room -- so a chunk the rules do not move does not
    // put a copy of its three alpha maps into the undo step. The before-image is taken at the same
    // instant, which is the last moment at which it is still the chunk's original state.
    bool touched = false;

    // Set once this chunk has proved it has no layer to spare; see resolve().
    bool eviction_failed = false;

    auto const ensure_registered = [&]
    {
      if (touched)
      {
        return;
      }

      captureFingerprint(texture_set, _before);
      action->registerChunkTextureChange(chunk);
      touched = true;
    };

    // A chunk that is already nothing but the winning texture has nothing to paint, and
    // paintTexture agrees -- it returns early on nTextures == 1 (texture_set.cpp:830), with no
    // alpha map in existence for the strength to be written into. Caught here so the commonest
    // no-op costs nothing at all; the general case is caught after the fact by comparing the two
    // fingerprints, because a chunk with two or more layers can be repainted with exactly the
    // values it already holds and neither paintTexture nor apply_alpha_changes will say so.
    auto const already_done = [&] (TerrainRuleResult const& decision)
    {
      return texture_set->num() == 1
          && std::string_view(texture_set->filename(0)) == decision.texture;
    };

    // The texture reference to paint a decision with, or null when this chunk cannot take it. Not
    // an error: four MCLY slots is a hard limit. Re-asked per unit because a paint can free or
    // fill a slot.
    auto const resolve = [&] (TerrainRuleResult const& decision) -> scoped_blp_texture_reference*
    {
      auto const found = _texture_refs.find(std::string(decision.texture));

      if (found == _texture_refs.end())
      {
        return nullptr;
      }

      if (texture_set->canPaintTexture(found->second))
      {
        return &found->second;
      }

      // canPaintTexture answers "already here, or is there a spare slot" and stops there
      // (texture_set.cpp:271-286). The paint does not stop there: get_texture_index_or_add drops
      // layers that paint nothing before it gives up (texture_set.cpp:396). Mirroring that is what
      // makes the preview's promise -- "layers painting nothing are dropped there to make room" --
      // true. Without it the preview counts the chunk under chunks_needing_eviction, says the
      // unused layer will go, and then every unit on that chunk is refused with no way to find it
      // afterwards.
      //
      // Asked once per chunk, not once per unit. A chunk whose four layers are all in use fails
      // this for every one of its 64 units, and each failed attempt still walks every texel of
      // every layer (texture_set.cpp:308-346). Nothing between two units can turn a failure into a
      // success either: the only thing that frees a slot is paintTexture's own trailing
      // eraseUnusedTextures, and after that canPaintTexture answers true above without ever
      // reaching here.
      if (eviction_failed)
      {
        return nullptr;
      }

      // Registered first: eraseUnusedTextures rewrites the layer list in place, so the undo entry
      // has to exist before it runs.
      ensure_registered();

      if (!texture_set->eraseUnusedTextures())
      {
        eviction_failed = true;
        return nullptr;
      }

      return texture_set->canPaintTexture(found->second) ? &found->second : nullptr;
    };

    // The one place a texture layer weight is written, and paintTexture owns all of it: adding or
    // evicting the layer (get_texture_index_or_add), creating the in-flight float alpha maps, and
    // rescaling the other layers so the texel still sums to 255. Strength is on the same 0-255
    // scale as the Texturing tool's brush level (texturing_tool.cpp:816), and pressure is 1
    // because a rule is an assignment rather than a stroke that builds up.
    auto const paint = [&] ( TerrainRuleResult const& decision
                           , scoped_blp_texture_reference& texture
                           , glm::vec3 const& position
                           , Brush& brush
                           )
    {
      chunk->paintTexture(position, &brush, static_cast<float>(decision.alpha), 1.0f, texture);
    };

    // Held per chunk and only added to the run's totals once the chunk is known to have really
    // moved. A chunk that ends where it started contributes to units_unchanged instead, so the
    // closing message never claims work that did not happen.
    std::size_t chunk_units_painted = 0;
    bool one_brush_pass = false;

    if (uniform)
    {
      if (already_done(decisions[0]))
      {
        _stats.units_unchanged += UNITS_PER_CHUNK;
        return;
      }

      if (scoped_blp_texture_reference* texture = resolve(decisions[0]))
      {
        glm::vec3 const centre
          (chunk->xbase + CHUNKSIZE * 0.5f, chunk->ybase, chunk->zbase + CHUNKSIZE * 0.5f);

        ensure_registered();
        paint(decisions[0], *texture, centre, _chunk_brush);

        chunk_units_painted += UNITS_PER_CHUNK;
        one_brush_pass = true;
      }
      else
      {
        _stats.units_refused += UNITS_PER_CHUNK;
      }
    }
    else
    {
      for (int unit_row = 0; unit_row < Collector::CHUNK_INNER_SIDE; ++unit_row)
      {
        for (int unit_col = 0; unit_col < Collector::CHUNK_INNER_SIDE; ++unit_col)
        {
          auto const& decision = decisions[unit_row * Collector::CHUNK_INNER_SIDE + unit_col];

          // Unmatched units are the ones the preview counted as unclaimed. Leaving them untouched
          // is the documented behaviour, not an oversight.
          if (!decision.matched)
          {
            continue;
          }

          if (already_done(decision))
          {
            ++_stats.units_unchanged;
            continue;
          }

          scoped_blp_texture_reference* texture = resolve(decision);

          if (!texture)
          {
            ++_stats.units_refused;
            continue;
          }

          ensure_registered();

          paint(decision, *texture, unitCentre(chunk, unit_row, unit_col), _unit_brush);

          ++chunk_units_painted;
        }
      }
    }

    if (!touched)
    {
      return;
    }

    // Committed per chunk rather than left for the save path. An uncommitted stroke keeps a
    // tmp_edit_alpha_values alive -- 4 planes of 4096 floats, 64 KiB -- and the Action caches a
    // copy of it for both the before and the after state, so deferring across a tile would cost
    // about 48 MiB for nothing. apply_alpha_changes also raises the ALPHAMAP update flag and the
    // lod-map dirty bit (texture_set.cpp:1676).
    texture_set->apply_alpha_changes();

    // The after-image, and the only trustworthy answer to "did this chunk actually move".
    // Re-applying a rule set that has already been applied repaints every multi-layer chunk with
    // the values it is already holding: paintTexture reports that as a change, and taking its word
    // for it marks the tile unsaved and tells the user thousands of units were painted when the
    // file on disk would come out identical.
    captureFingerprint(texture_set, _after);

    if (_after == _before)
    {
      _stats.units_unchanged += chunk_units_painted;
      ++_stats.chunks_unchanged;
      return;
    }

    _stats.units_painted += chunk_units_painted;
    _stats.chunks_uniform += one_brush_pass ? 1 : 0;
    ++_stats.chunks_painted;

    // registerChunkUpdate is a render-refresh flag and cannot mean "unsaved" -- every chunk that
    // loads sets the same bits. The save path tests MapTile::changed instead (map_index.cpp:555),
    // so without this the ADT is skipped by saveChanged and the whole run is lost on unload.
    _world->mapIndex.setChanged(tile);
  }
}
