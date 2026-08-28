// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/rendering/DetailDoodadPlacement.hpp>

#include <noggit/DBC.h>
#include <noggit/MapChunk.h>
#include <noggit/MapHeaders.h>
#include <noggit/MapTile.h>
#include <noggit/texture_set.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace
{
  // Half of one 8x8 cell of a chunk. UNITSIZE is CHUNKSIZE / 8 == 533.33333 / 16 / 8 ==
  // 4.1666666, which is the 4.16667 the client transcription hardcodes as `unit`, and half of it
  // is its `half` of 2.08333. Taken from the shared constant instead of retyped so the two cannot
  // drift.
  constexpr float CELL_HALF = UNITSIZE * 0.5f;

  // Reject a doodad whose fan triangle leans more than this away from vertical. 0.4 is the
  // client's threshold, carried over from the transcription; acos(0.4) is 66.4 degrees, so grass
  // stops at the point where terrain reads as a cliff rather than a slope.
  constexpr float MIN_SURFACE_UP = 0.4f;

  // GroundEffectTexture.Amount is a uint the set editor lets a user drive to 255
  // (GroundEffectsTool.cpp:322), and it multiplies against up to 256 cell picks. These two caps
  // are the only thing standing between a hand-authored set and a chunk that allocates a hundred
  // thousand matrices. Real 3.3.5 data does not come close: the DBC's own values are single
  // digits, so nothing shipping with the client is altered by either cap.
  constexpr unsigned MAX_DOODADS_PER_CELL = 64;
  constexpr std::size_t MAX_DOODADS_PER_CHUNK = 2048;

  // Same reason, for the weight columns: the inner fill loop below runs `weight` times.
  constexpr unsigned MAX_SLOT_WEIGHT = 16;

  // How far a doodad may lean off its surface normal, and how much its size may vary. Both are
  // driven by the client's own random draws -- the transcription labels the three values it takes
  // per doodad "[0] = random tilt, [1] = scale, [2] = y[aw]" -- but the MAGNITUDES here are ours.
  // Nobody has measured the client's, and these are chosen to read as natural variation rather
  // than to claim a match: 6 degrees of lean and 10% of size.
  constexpr float TILT_LIMIT_RADIANS = 0.10472f; // 6 degrees
  constexpr float SCALE_VARIANCE = 0.10f;

  // The client's noise table, 256 bytes, a permutation of 0..255 -- verified: 256 entries, 256
  // distinct values, none of 0..255 missing. It is transcribed in this repository already, but
  // commented out and replaced with the single byte 'a'
  // (ChunkAddDetailDoodads.cpp:50-64), which makes every shuffle there an out-of-bounds read.
  //
  // The last three entries repeat the first three. The shuffle reads a 32-bit word at a byte index
  // that runs to 255, so indices 253, 254 and 255 need three bytes past the end; repeating the
  // start is exactly what a modulo-256 table read would return, and it turns the read into a plain
  // in-bounds one instead of the undefined behaviour it would otherwise be.
  constexpr std::uint8_t NOISE[259] =
  {
    0x8E, 0x14, 0x27, 0x99, 0xFD, 0xAA, 0xC7, 0x08, 0xD5, 0xE6, 0x3E, 0x1F,
    0xF6, 0xBB, 0x55, 0xDA, 0x75, 0xA0, 0x4A, 0x6A, 0xE8, 0xBD, 0x97, 0xFF,
    0xDE, 0x9B, 0xBC, 0x9F, 0x81, 0x8A, 0xA1, 0x46, 0x6E, 0x0B, 0xE3, 0x63,
    0x76, 0x7A, 0x6C, 0x5D, 0x88, 0xD3, 0x69, 0xCA, 0xC3, 0x47, 0xB9, 0x25,
    0x83, 0xAB, 0xA2, 0x3F, 0xA6, 0x41, 0x7C, 0xBA, 0xE5, 0xAC, 0x95, 0x01,
    0x7E, 0xCF, 0x09, 0xC1, 0xD9, 0x62, 0x70, 0x71, 0x8D, 0xDB, 0x05, 0x02,
    0x24, 0x87, 0xEF, 0x54, 0xC6, 0xD4, 0x37, 0x30, 0xD0, 0x1B, 0xCB, 0x7B,
    0xB8, 0xE4, 0xD8, 0xEC, 0x49, 0xCE, 0xAD, 0xDC, 0x13, 0xA9, 0x94, 0xC4,
    0x8F, 0x39, 0xAE, 0x0D, 0x18, 0x52, 0xDD, 0x0E, 0x78, 0xFA, 0xF5, 0x85,
    0x58, 0xD2, 0xAF, 0x6D, 0xA4, 0xB2, 0x53, 0x3B, 0x51, 0xA5, 0x50, 0xBE,
    0xFC, 0x2D, 0xF4, 0x11, 0x48, 0x98, 0x16, 0xF1, 0x86, 0xDF, 0x3D, 0x66,
    0x5E, 0x44, 0x2E, 0x2F, 0x36, 0x07, 0x6B, 0x17, 0x8B, 0x29, 0x4C, 0xB6,
    0xE2, 0x89, 0x5F, 0xE7, 0xCD, 0xA7, 0x21, 0xE1, 0x4D, 0xC9, 0x65, 0xED,
    0xFE, 0xEE, 0x9C, 0x23, 0x33, 0x7D, 0xB7, 0x04, 0x9E, 0x9A, 0x2A, 0x40,
    0xB3, 0x10, 0x5B, 0xF3, 0x82, 0x77, 0x1C, 0x92, 0x20, 0x4E, 0x1E, 0x57,
    0x22, 0x72, 0x06, 0x8C, 0x67, 0x2C, 0x73, 0xFB, 0x59, 0xC2, 0x0A, 0xBF,
    0x79, 0x5C, 0xF9, 0x0C, 0x28, 0x1A, 0x12, 0x68, 0x74, 0x34, 0x19, 0x42,
    0xB1, 0xC0, 0x84, 0xF8, 0x38, 0xF0, 0x15, 0x9D, 0x60, 0xF2, 0x3A, 0x6F,
    0xB4, 0x90, 0xEB, 0x91, 0x1D, 0x7F, 0x35, 0x61, 0x5A, 0x32, 0x03, 0x56,
    0xA3, 0xC5, 0x2B, 0x93, 0x80, 0x0F, 0x4B, 0x43, 0xF7, 0xA8, 0xE0, 0x3C,
    0x96, 0xD1, 0x64, 0x26, 0xD7, 0x45, 0xCC, 0x4F, 0xC8, 0xB0, 0xE9, 0xB5,
    0x00, 0xD6, 0x31, 0xEA, 0x8E, 0x14, 0x27
  };

  constexpr std::uint32_t rotateLeft(std::uint32_t value, unsigned bits)
  {
    // The transcription writes the shift as `val >> (-len & 31)`, which happens to give 0 for
    // len == 0 and therefore `val | val`. Spelled out, because a reader should not have to prove
    // that a negative shift count is not being formed.
    return bits == 0u ? value : ((value << bits) | (value >> (32u - bits)));
  }

  // Little-endian by construction rather than by reinterpret_cast, which is what the
  // transcription does (`*(uint32_t*)(&_noise[a])`). That cast both violates strict aliasing and
  // makes the generator's output depend on the host's byte order -- which would mean the preview
  // placed grass in different spots on a big-endian build than the client does.
  constexpr std::uint32_t noiseWord(std::uint8_t index)
  {
    return static_cast<std::uint32_t>(NOISE[index])
         | (static_cast<std::uint32_t>(NOISE[index + 1]) << 8)
         | (static_cast<std::uint32_t>(NOISE[index + 2]) << 16)
         | (static_cast<std::uint32_t>(NOISE[index + 3]) << 24);
  }

  // The client's detail doodad generator.
  //
  // UNVERIFIED CONSTANT, and it is the one place in this file where nobody should assume parity.
  // The transcription carries a table it declares and never uses:
  //
  //     _sumPairs { {-28, 216}, {-24, 212}, {-12, 200}, {-4, 184} }
  //
  // The four subtrahends are the four this shuffle applies. The four companions read like wrap
  // constants -- "if the byte went negative, add this back" -- which would keep every sub-seed
  // inside a fixed range instead of letting it wrap at 256. What the code actually EXECUTES is the
  // plain 8-bit wrap reproduced below, and that is what is reproduced here, because it is the only
  // version anyone here can read. If a future comparison against the client shows the sumPairs
  // form is the real one, this function is the single place that changes, and nothing else in this
  // file has to move.
  class DetailDoodadRandom
  {
  public:
    explicit DetailDoodadRandom(std::uint32_t source)
    {
      _source = source;

      // 47, 53, 59 and 61 -- four primes, each result parked in its own byte at a two-bit offset.
      _seed = ((source % 0x2Fu) << 26)
            | ((source % 0x35u) << 18)
            | ((source % 0x3Bu) << 10)
            | ((source % 0x3Du) << 2);
    }

    std::uint32_t shuffle()
    {
      std::uint8_t const a = static_cast<std::uint8_t>(((_seed >>  0) & 0xFFu) - 0x1Cu);
      std::uint8_t const b = static_cast<std::uint8_t>(((_seed >>  8) & 0xFFu) - 0x18u);
      std::uint8_t const c = static_cast<std::uint8_t>(((_seed >> 16) & 0xFFu) - 0x0Cu);
      std::uint8_t const d = static_cast<std::uint8_t>(((_seed >> 24) & 0xFFu) - 0x04u);

      _seed = static_cast<std::uint32_t>(a)
            | (static_cast<std::uint32_t>(b) << 8)
            | (static_cast<std::uint32_t>(c) << 16)
            | (static_cast<std::uint32_t>(d) << 24);

      _source += rotateLeft(noiseWord(a), 0)
               ^ rotateLeft(noiseWord(d), 1)
               ^ rotateLeft(noiseWord(c), 2)
               ^ rotateLeft(noiseWord(b), 3);

      return _source;
    }

    // The client's genCoord: one shuffle turned into a float in (-1, 1] by pasting 23 random
    // mantissa bits onto exponent 127, which gives [1, 2), and then folding that around 2 in the
    // direction the sign bit picks.
    float coord()
    {
      std::uint32_t const roll = shuffle();
      std::uint32_t const bits = (roll & 0x007FFFFFu) | 0x3F800000u;

      float mantissa;
      std::memcpy(&mantissa, &bits, sizeof(float));

      return (roll & 0x80000000u) ? (2.0f - mantissa) : (mantissa - 2.0f);
    }

  private:
    std::uint32_t _source;
    std::uint32_t _seed;
  };

  // One GroundEffectTexture row, resolved once per chunk instead of once per cell.
  //
  // Hoisting this out of the cell loop is pure memoisation -- the sixteen slots depend only on the
  // row -- but it is the difference between four DBC lookups per chunk and up to 256, each of
  // which is a map lookup that throws on a miss.
  struct EffectDefinition
  {
    bool valid = false;
    unsigned amount = 8;
    std::array<std::uint32_t, 16> doodad_slots{};
  };

  EffectDefinition resolveEffect(unsigned effect_id)
  {
    EffectDefinition definition;

    // 0xFFFFFFFF is layer_info::effectID's default for "this layer names no ground effect"
    // (texture_set.hpp:39); 0 is what an MCLY that was written with none carries.
    if (effect_id == 0u || effect_id == 0xFFFFFFFFu)
    {
      return definition;
    }

    try
    {
      DBCFile::Record const record = gGroundEffectTextureDB.getByID(effect_id);

      definition.amount = record.getUInt(GroundEffectTextureDB::Amount);

      // The client's own fallback when a row leaves Amount at zero.
      if (!definition.amount)
      {
        definition.amount = 8u;
      }

      definition.amount = std::min(definition.amount, MAX_DOODADS_PER_CELL);

      // The client's weighted lottery: sixteen slots filled by walking in steps of 13. 13 and 16
      // are coprime, so the step visits all sixteen slots before repeating, which is what spreads
      // one doodad's share of the draw across the table rather than leaving it in one run.
      unsigned value = 0u;
      unsigned accumulated_weight = 0u;

      for (std::size_t i = 0; i < 4; ++i)
      {
        std::uint32_t const doodad_id = record.getUInt(GroundEffectTextureDB::Doodads + i);
        unsigned const weight = std::min(record.getUInt(GroundEffectTextureDB::Weights + i)
                                        , MAX_SLOT_WEIGHT);

        unsigned slot = value;

        for (unsigned j = 0; j < weight; ++j)
        {
          definition.doodad_slots[slot & 15u] = doodad_id;
          slot += 13u;
        }

        accumulated_weight += weight;
        value += weight * 13u;
      }

      // Whatever the four weights left unclaimed goes to one doodad column. The transcription
      // writes `Doodads + accumWeight & 3u`, which parses as `(Doodads + accumWeight) & 3` --
      // Doodads is 1, so it reads column (1 + accumWeight) & 3, and for accumulated weights of 3,
      // 7, 11 or 15 that is column 0 by accident rather than by intent. Parenthesised here so it
      // names a doodad column.
      for (unsigned i = accumulated_weight; i < 16u; ++i)
      {
        definition.doodad_slots[value & 15u]
          = record.getUInt(GroundEffectTextureDB::Doodads + (accumulated_weight & 3u));
        value += 13u;
      }

      definition.valid = true;
    }
    catch (DBCFile::NotFound const&)
    {
      // A chunk naming a ground effect id that is not in the DBC is ordinary on a modified client
      // and is not worth a log line per chunk per rebuild. It draws nothing, which is the honest
      // answer.
    }

    return definition;
  }

  // The four fan triangles of one 8x8 cell, as unit normals, plus the cell's centre vertex.
  //
  // Terrain is a heightfield, so a triangle's normal always has a positive Y once wound correctly;
  // the winding used below is the reverse of the transcription's, whose fanIndices traverse the
  // same four edges in the opposite direction and therefore produce downward normals. Same edges,
  // opposite sense -- which is how the 17/18 correction to those indices was confirmed.
  struct CellSurface
  {
    bool built = false;
    glm::vec3 centre{0.0f, 0.0f, 0.0f};
    glm::vec3 normals[4]{};
  };

  // Quadrant of the cell a centre-relative offset falls in, matching the fan: 0 is the -Z
  // triangle, 1 the +X, 2 the +Z, 3 the -X.
  int fanQuadrant(float dx, float dz)
  {
    if (std::fabs(dx) >= std::fabs(dz))
    {
      return dx >= 0.0f ? 1 : 3;
    }

    return dz >= 0.0f ? 2 : 0;
  }

  void buildCellSurface(glm::vec3 const* heightmap, unsigned cell_x, unsigned cell_z
                       , CellSurface& surface)
  {
    // MapChunk's heightmap is 17 alternating rows of 9 outer and 8 inner vertices
    // (MapChunk.cpp:176-186). From the outer vertex at (cell_x, cell_z) the cell's four corners
    // are +0, +1, +17 and +18, and its centre vertex is +9.
    glm::vec3 const* const origin = heightmap + (cell_z * 17u + cell_x);

    glm::vec3 const& c00 = origin[0];
    glm::vec3 const& c10 = origin[1];
    glm::vec3 const& c01 = origin[17];
    glm::vec3 const& c11 = origin[18];

    surface.centre = origin[9];

    // Ordered so that cross(first - centre, second - centre) points up on flat ground. Checked by
    // hand for the -Z triangle: u = (+h, 0, -h), v = (-h, 0, -h) gives a Y of
    // u.z*v.x - u.x*v.z = (-h)(-h) - (h)(-h) = 2h^2 > 0.
    glm::vec3 const* const fan[4][2] =
      { { &c10, &c00 }   // -Z
      , { &c11, &c10 }   // +X
      , { &c01, &c11 }   // +Z
      , { &c00, &c01 }   // -X
      };

    for (int i = 0; i < 4; ++i)
    {
      glm::vec3 const normal
        (glm::cross(*fan[i][0] - surface.centre, *fan[i][1] - surface.centre));

      float const length = glm::length(normal);

      // A degenerate triangle can only come from a heightmap with coincident vertices. Straight up
      // makes it flat ground, which passes the slope filter and evaluates to the centre height --
      // the least surprising thing to do with a cell that has no surface to speak of.
      surface.normals[i] = length > 1e-6f ? (normal / length) : glm::vec3(0.0f, 1.0f, 0.0f);
    }

    surface.built = true;
  }
}

namespace Noggit::Rendering
{
  std::vector<DetailDoodadGroup> buildChunkDetailDoodads(MapChunk* chunk, unsigned density)
  {
    std::vector<DetailDoodadGroup> groups;

    if (!chunk || !chunk->texture_set || !chunk->mt)
    {
      return groups;
    }

    density = std::clamp(density, DETAIL_DOODAD_MIN_DENSITY, DETAIL_DOODAD_MAX_DENSITY);

    TextureSet* const texture_set = chunk->texture_set.get();

    // Resolve the four layers' effect rows once. A chunk with no layer naming a ground effect
    // produces nothing at all, and finding that out here costs four DBC lookups rather than a full
    // placement pass.
    std::array<EffectDefinition, 4> layer_effects;
    bool any_effect = false;

    for (std::size_t layer = 0; layer < 4; ++layer)
    {
      layer_effects[layer] = resolveEffect(texture_set->getEffectForLayer(layer));
      any_effect = any_effect || layer_effects[layer].valid;
    }

    if (!any_effect)
    {
      return groups;
    }

    // The client seeds per chunk from its global position on the map, which is what makes a
    // chunk's grass the same every time you look at it and different from its neighbour's.
    DetailDoodadRandom random
      ( (static_cast<std::uint32_t>(chunk->mt->index.z * 16u + chunk->py) << 16u)
      | static_cast<std::uint32_t>(chunk->mt->index.x * 16u + chunk->px)
      );

    // Pass one: pick cells, and build the fan surface of each cell the first time it comes up.
    //
    // The two draws happen before the already-visited test, so a repeated pick still advances the
    // generator. That is what the transcription does and it matters: the whole stream downstream
    // shifts if it does not.
    std::vector<std::array<unsigned, 2>> picks(density);
    std::array<CellSurface, 64> surfaces{};
    std::uint64_t visited = 0ull;

    for (unsigned pick = 0; pick < density; ++pick)
    {
      picks[pick][0] = random.shuffle() & 7u;
      picks[pick][1] = random.shuffle() & 7u;

      unsigned const cell = picks[pick][1] * 8u + picks[pick][0];

      // 1ull, not 1u: `cell` reaches 63 and shifting a 32-bit 1 that far is undefined behaviour,
      // which is what the transcription's `1u << ...` into a std::size_t does.
      std::uint64_t const bit = 1ull << cell;

      if (visited & bit)
      {
        continue;
      }

      visited |= bit;

      buildCellSurface(chunk->getHeightmap(), picks[pick][0], picks[pick][1], surfaces[cell]);
    }

    // Pass two: emit the doodads.
    std::array<std::uint8_t, 8> const& stencil = texture_set->_doodadStencil;
    std::array<std::uint16_t, 8> const& mapping = texture_set->getDoodadMapping();
    unsigned const holes = chunk->getHoleMask();

    // Keyed by GroundEffectDoodad row so the caller resolves each model once. At most sixteen
    // distinct rows can come out of one chunk -- four layers of four doodad columns -- so a linear
    // scan is cheaper here than any map.
    std::size_t emitted = 0;

    for (unsigned pick = 0; pick < density && emitted < MAX_DOODADS_PER_CHUNK; ++pick)
    {
      unsigned const cell_x = picks[pick][0];
      unsigned const cell_z = picks[pick][1];

      // THE EXCLUSION MAP. _doodadStencil is one bit per cell, set means "no doodads here", and it
      // is what the Ground Effects tool's exclusion brush paints
      // (texture_set.cpp:462-489). Honouring it is not optional: a user who painted an exclusion
      // and then saw foliage standing in it would have been shown a lie about their own edit.
      //
      // Note what skipping here does to the generator. The per-doodad draws below never happen for
      // an excluded cell, so painting an exclusion also re-rolls every doodad picked after it in
      // the chunk. That is the transcription's order and therefore the client's: the exclusion map
      // is baked data to the client, which has no notion of it changing under the same seed.
      if (stencil[cell_z] & (1u << cell_x))
      {
        continue;
      }

      // Holes are 4x4 over the chunk while these cells are 8x8, so both indices halve. The
      // transcription's `holeMask[splat[1] / 2 * 4 + splat[0]]` halves only Z and then indexes a
      // 16-element array with a value that reaches 19.
      if (holes & (1u << ((cell_z / 2u) * 4u + (cell_x / 2u))))
      {
        continue;
      }

      // Two bits per cell, naming which of the four texture layers owns this cell's doodads. This
      // is the MCLY "predTex" field the client reads; it is not recomputed from the alphamaps here,
      // because the client does not recompute it either.
      unsigned const layer = (mapping[cell_z] >> (2u * cell_x)) & 3u;
      EffectDefinition const& effect = layer_effects[layer];

      if (!effect.valid)
      {
        continue;
      }

      CellSurface const& surface = surfaces[cell_z * 8u + cell_x];

      if (!surface.built)
      {
        continue;
      }

      for (unsigned i = 0; i < effect.amount && emitted < MAX_DOODADS_PER_CHUNK; ++i)
      {
        // Draw order below is the transcription's exactly, including which early-outs happen
        // before the three-value draw and therefore leave the generator where it was. Reordering
        // any of it moves every doodad after it.
        float const offset_x = random.coord() * CELL_HALF;
        float const offset_z = random.coord() * CELL_HALF;

        // Constant for the whole cell, which is how a patch of one plant type forms. `amount` is
        // fixed per effect, so across a chunk this walks the slot table by pick index.
        std::uint32_t const doodad_id = effect.doodad_slots[(effect.amount + pick) & 15u];

        if (!doodad_id)
        {
          continue;
        }

        glm::vec3 const& normal = surface.normals[fanQuadrant(offset_x, offset_z)];

        if (normal.y < MIN_SURFACE_UP)
        {
          continue;
        }

        // Height on the plane of that fan triangle, which is the plane the viewport is drawing.
        // normal.y is at least MIN_SURFACE_UP here, so the division is safe.
        float const height = surface.centre.y
                           - (normal.x * offset_x + normal.z * offset_z) / normal.y;

        glm::vec3 const position( surface.centre.x + offset_x
                                , height
                                , surface.centre.z + offset_z
                                );

        float const tilt = random.coord() * TILT_LIMIT_RADIANS;
        float const scale = 1.0f + random.coord() * SCALE_VARIANCE;
        float const yaw = random.coord() * 3.14159265f;

        // Stand the model along the surface normal, spin it about that normal, then lean it a
        // little. Building the matrix directly rather than through SceneObject's euler path
        // because a detail doodad has no MDDF rotation to honour -- there is no stored orientation
        // for the -90 degree convention that path applies to correct.
        glm::mat4x4 transform = glm::translate(glm::mat4x4(1.0f), position);

        glm::vec3 const axis(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), normal));
        float const axis_length = glm::length(axis);

        if (axis_length > 1e-5f)
        {
          transform = glm::rotate(transform, std::acos(std::min(normal.y, 1.0f))
                                 , axis / axis_length);
        }

        transform = glm::rotate(transform, yaw, glm::vec3(0.0f, 1.0f, 0.0f));
        transform = glm::rotate(transform, tilt, glm::vec3(1.0f, 0.0f, 0.0f));
        transform = glm::scale(transform, glm::vec3(scale, scale, scale));

        auto group (std::find_if(groups.begin(), groups.end()
                                , [doodad_id] (DetailDoodadGroup const& candidate)
                                  { return candidate.doodad_id == doodad_id; }));

        if (group == groups.end())
        {
          groups.push_back(DetailDoodadGroup{doodad_id, {}});
          group = std::prev(groups.end());
        }

        group->transforms.push_back(transform);
        ++emitted;
      }
    }

    return groups;
  }
}
