// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/DBC.h>
#include <noggit/liquid_layer.hpp>
#include <noggit/Log.h>
#include <noggit/MapChunk.h>
#include <noggit/Misc.h>
#include <opengl/scoped.hpp>
#include <ClientFile.hpp>

#include <util/sExtendableArray.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace
{
  inline glm::vec2 default_uv(int px, int pz)
  {
    return {static_cast<float>(px) / 4.f, static_cast<float>(pz) / 4.f};
  }
}

liquid_layer::liquid_layer(ChunkWater* chunk, glm::vec3 const& base, float height, int liquid_id)
  : _liquid_id(liquid_id)
  , _liquid_vertex_format(LVF_HEIGHT_DEPTH)
  , _minimum(height)
  , _maximum(height)
  , _subchunks(0)
  , pos(base)
  , _chunk(chunk)
{
  if (!gLiquidTypeDB.CheckIfIdExists(_liquid_id))
    _liquid_id = LIQUID_WATER;

  create_vertices(height);

  changeLiquidID(_liquid_id);
  
  update_min_max();
}

liquid_layer::liquid_layer(ChunkWater* chunk, glm::vec3 const& base, mclq& liquid, int liquid_id)
  : _liquid_id(liquid_id)
  , _minimum(liquid.min_height)
  , _maximum(liquid.max_height)
  , _subchunks(0)
  , pos(base)
  , _chunk(chunk)
{
  if (!gLiquidTypeDB.CheckIfIdExists(_liquid_id))
    _liquid_id = LIQUID_WATER;

  changeLiquidID(_liquid_id);

  for (int z = 0; z < 8; ++z)
  {
    for (int x = 0; x < 8; ++x)
    {
      misc::set_bit(_subchunks, x, z, !liquid.tiles[z * 8 + x].dont_render);
    }
  }

  for (int z = 0; z < 9; ++z)
  {
    for (int x = 0; x < 9; ++x)
    {
      const unsigned v_index = z * 9 + x;
      mclq_vertex const& v = liquid.vertices[v_index];

      liquid_vertex lv;

      // _liquid_vertex_format is set by changeLiquidID()
      if (_liquid_vertex_format == LVF_HEIGHT_UV)
      {
        lv.depth = 1.f;
        lv.uv = { static_cast<float>(v.magma.x) / 255.f, static_cast<float>(v.magma.y) / 255.f };
      }
      else
      {
        lv.depth = static_cast<float>(v.water.depth) / 255.f;
        lv.uv = default_uv(x, z);
      }

      // sometimes there's garbage data on unused tiles that mess things up
      lv.position = { pos.x + UNITSIZE * x, std::clamp(v.height, _minimum, _maximum), pos.z + UNITSIZE * z };


      _vertices[v_index] = lv;
    }
  }
  update_min_max();
}

liquid_layer::liquid_layer(ChunkWater* chunk
                           , BlizzardArchive::ClientFile& f
                           , std::size_t base_pos
                           , glm::vec3 const& base
                           , MH2O_Information const& info
                           , std::uint64_t infomask)
  : _liquid_id(info.liquid_id)
  , _liquid_vertex_format(info.liquid_vertex_format)
  , _minimum(info.minHeight)
  , _maximum(info.maxHeight)
  , _subchunks(0)
  , pos(base)
  , _chunk(chunk)
{
  // check if liquid id is valid or some downported maps will crash
  if (!gLiquidTypeDB.CheckIfIdExists(_liquid_id))
    _liquid_id = LIQUID_WATER;

  int offset = 0;
  for (int z = 0; z < info.height; ++z)
  {
    for (int x = 0; x < info.width; ++x)
    {
      setSubchunk(x + info.xOffset, z + info.yOffset, (infomask >> offset) & 1);
      offset++;
    }
  }

  // default values
  create_vertices(_minimum);

  if (info.ofsHeightMap)
  {
    f.seek(base_pos + info.ofsHeightMap);

    if (_liquid_vertex_format == LVF_HEIGHT_DEPTH || _liquid_vertex_format == LVF_HEIGHT_UV)
    {

      for (int z = info.yOffset; z <= info.yOffset + info.height; ++z)
      {
        for (int x = info.xOffset; x <= info.xOffset + info.width; ++x)
        {
            float h;
            f.read(&h, sizeof(float));

            _vertices[z * 9 + x].position.y = std::clamp(h, _minimum, _maximum);
        }
      }
    }

    if (_liquid_vertex_format == LVF_HEIGHT_UV)
    {
      for (int z = info.yOffset; z <= info.yOffset + info.height; ++z)
      {
        for (int x = info.xOffset; x <= info.xOffset + info.width; ++x)
        {
          mh2o_uv uv;
          f.read(&uv, sizeof(mh2o_uv));
          _vertices[z * 9 + x].uv =
            { static_cast<float>(uv.x) / 255.f
            , static_cast<float>(uv.y) / 255.f
            };
        }
      }
    }

    if (_liquid_vertex_format == LVF_HEIGHT_DEPTH || _liquid_vertex_format == LVF_DEPTH)
    {
      for (int z = info.yOffset; z <= info.yOffset + info.height; ++z)
      {
        for (int x = info.xOffset; x <= info.xOffset + info.width; ++x)
        {
          std::uint8_t depth;
          f.read(&depth, sizeof(std::uint8_t));
          _vertices[z * 9 + x].depth = static_cast<float>(depth) / 255.f;
        }
      }
    }
  }

  changeLiquidID(_liquid_id); // to update the liquid type

  update_min_max();
}

liquid_layer::liquid_layer(liquid_layer&& other) noexcept
  : _liquid_id(other._liquid_id)
  , _liquid_vertex_format(other._liquid_vertex_format)
  , _minimum(other._minimum)
  , _maximum(other._maximum)
  // , _center(other._center)
  , _subchunks(other._subchunks)
  , _vertices(other._vertices)
  // , _indices_by_lod(other._indices_by_lod)
  , _fatigue_enabled(other._fatigue_enabled)
  , pos(other.pos)
  , _chunk(other._chunk)
{
  // update liquid type and vertex format
  changeLiquidID(_liquid_id);
}

liquid_layer::liquid_layer(liquid_layer const& other)
  : _liquid_id(other._liquid_id)
  , _liquid_vertex_format(other._liquid_vertex_format)
  , _minimum(other._minimum)
  , _maximum(other._maximum)
  , _subchunks(other._subchunks)
  , _vertices(other._vertices)
  // , _indices_by_lod(other._indices_by_lod)
  , _fatigue_enabled(other._fatigue_enabled)
  , pos(other.pos)
  , _chunk(other._chunk)
{
  // update liquid type and vertex format
  changeLiquidID(_liquid_id);
}

liquid_layer& liquid_layer::operator= (liquid_layer&& other) noexcept
{
  std::swap(_liquid_id, other._liquid_id);
  std::swap(_liquid_vertex_format, other._liquid_vertex_format);
  std::swap(_minimum, other._minimum);
  std::swap(_maximum, other._maximum);
  std::swap(_subchunks, other._subchunks);
  std::swap(_vertices, other._vertices);
  std::swap(_fatigue_enabled, other._fatigue_enabled);
  std::swap(pos, other.pos);
  // std::swap(_indices_by_lod, other._indices_by_lod);
  std::swap(_chunk, other._chunk);

  // update liquid type and vertex format
  changeLiquidID(_liquid_id);
  other.changeLiquidID(other._liquid_id);

  return *this;
}

liquid_layer& liquid_layer::operator=(liquid_layer const& other)
{

  _liquid_vertex_format = other._liquid_vertex_format;
  _minimum = other._minimum;
  _maximum = other._maximum;
  _subchunks = other._subchunks;
  _vertices = other._vertices;
  pos = other.pos;
  // _indices_by_lod = other._indices_by_lod;
  _fatigue_enabled = other._fatigue_enabled;
  _chunk = other._chunk;

  // update liquid type and vertex format
  changeLiquidID(other._liquid_id);
  return *this;
}

void liquid_layer::create_vertices(float height)
{
    int index = 0;
    for (int z = 0; z < 9; ++z)
    {
        const float posZ = pos.z + UNITSIZE * z;
        for (int x = 0; x < 9; ++x, ++index)
        {
            _vertices[index] = liquid_vertex( glm::vec3(pos.x + UNITSIZE * x, height, posZ)
                , default_uv(x, z)
                , 1.f
            );
        }
    }
}

void liquid_layer::save(util::sExtendableArray& adt, int base_pos, int& info_pos, int& current_pos) const
{
  int min_x = 9, min_z = 9, max_x = 0, max_z = 0;
  bool filled = true;

  for (int z = 0; z < 8; ++z)
  {
    for (int x = 0; x < 8; ++x)
    {
      if (hasSubchunk(x, z))
      {
        min_x = std::min(x, min_x);
        min_z = std::min(z, min_z);
        max_x = std::max(x + 1, max_x);
        max_z = std::max(z + 1, max_z);
      }
      else
      {
        filled = false;
      }
    }
  }

  MH2O_Information info;
  std::uint64_t mask = 0;

  info.liquid_id = _liquid_id;
  info.liquid_vertex_format = _liquid_vertex_format;
  info.minHeight = _minimum;
  info.maxHeight = _maximum;
  info.xOffset = min_x;
  info.yOffset = min_z;
  info.width = max_x - min_x;
  info.height = max_z - min_z;

  if (filled)
  {
    info.ofsInfoMask = 0;
  }
  else
  {
    std::uint64_t value = 1;
    for (int z = info.yOffset; z < info.yOffset + info.height; ++z)
    {
      for (int x = info.xOffset; x < info.xOffset + info.width; ++x)
      {
        if (hasSubchunk(x, z))
        {
          mask |= value;
        }
        value <<= 1;
      }
    }

    if (mask > 0)
    {
      info.ofsInfoMask = current_pos - base_pos;
      adt.Insert(current_pos, 8, reinterpret_cast<char*>(&mask));
      current_pos += 8;
    }
  }

  int vertices_count = (info.width + 1) * (info.height + 1);
  info.ofsHeightMap = current_pos - base_pos;

  if (_liquid_vertex_format == LVF_HEIGHT_DEPTH || _liquid_vertex_format == LVF_HEIGHT_UV)
  {
    adt.Extend(vertices_count * sizeof(float));

    for (int z = info.yOffset; z <= info.yOffset + info.height; ++z)
    {
      for (int x = info.xOffset; x <= info.xOffset + info.width; ++x)
      {
        memcpy(adt.GetPointer<char>(current_pos).get(), &_vertices[z * 9 + x].position.y, sizeof(float));
        current_pos += sizeof(float);
      }
    }
  }
  // no heightmap/depth data for fatigue chunks
  else if (_fatigue_enabled)
  {
      info.ofsHeightMap = 0;
  }

  if (_liquid_vertex_format == LVF_HEIGHT_UV)
  {
    adt.Extend(vertices_count * sizeof(mh2o_uv));

    for (int z = info.yOffset; z <= info.yOffset + info.height; ++z)
    {
      for (int x = info.xOffset; x <= info.xOffset + info.width; ++x)
      {
        mh2o_uv uv;
        uv.x = static_cast<std::uint16_t>(std::min(_vertices[z * 9 + x].uv.x * 255.f, 65535.f));
        uv.y = static_cast<std::uint16_t>(std::min(_vertices[z * 9 + x].uv.y * 255.f, 65535.f));

        memcpy(adt.GetPointer<char>(current_pos).get(), &uv, sizeof(mh2o_uv));
        current_pos += sizeof(mh2o_uv);
      }
    }
  }

  if (_liquid_vertex_format == LVF_HEIGHT_DEPTH || (_liquid_vertex_format == LVF_DEPTH && !_fatigue_enabled))
  {
    adt.Extend(vertices_count * sizeof(std::uint8_t));

    for (int z = info.yOffset; z <= info.yOffset + info.height; ++z)
    {
      for (int x = info.xOffset; x <= info.xOffset + info.width; ++x)
      {
          std::uint8_t depth = static_cast<std::uint8_t>(std::min(_vertices[z * 9 + x].depth * 255.0f, 255.f));
        memcpy(adt.GetPointer<char>(current_pos).get(), &depth, sizeof(std::uint8_t));
        current_pos += sizeof(std::uint8_t);
      }
    }
  }

  memcpy(adt.GetPointer<char>(info_pos).get(), &info, sizeof(MH2O_Information));
  info_pos += sizeof(MH2O_Information);
}

void liquid_layer::changeLiquidID(int id)
{
  _liquid_id = id;

  try
  {
    DBCFile::Record lLiquidTypeRow = gLiquidTypeDB.getByID(_liquid_id);

    _liquid_type = lLiquidTypeRow.getInt(LiquidTypeDB::Type);

    switch (_liquid_type)
    {
    case liquid_basic_types_magma:
      _mclq_liquid_type = mclq_liquid_magma;
      _liquid_vertex_format = LVF_HEIGHT_UV;
      break;
    case liquid_basic_types_slime:
      _mclq_liquid_type = mclq_liquid_slime;
      _liquid_vertex_format = LVF_HEIGHT_UV;
      break;
    case liquid_basic_types_ocean: // ocean
      // lvf 2 is only used for flat water at height 0
      _liquid_vertex_format = misc::float_equals(_minimum, 0.f) && misc::float_equals(_maximum, 0.f) ? LVF_DEPTH : LVF_HEIGHT_DEPTH;
      _mclq_liquid_type = mclq_liquid_ocean;
      break;
    default: // river
      _liquid_vertex_format = LVF_HEIGHT_DEPTH;
      _mclq_liquid_type = mclq_liquid_river;
      break;
    }
  }
  catch (LiquidTypeDB::NotFound)
  {
      assert(false);
      LogError << "Liquid type id " << _liquid_type << " not found in LiquidType dbc" << std::endl;
  }
}

void liquid_layer::crop(MapChunk* chunk)
{
  if (_maximum < chunk->getMinHeight())
  {
    _subchunks = 0;
  }
  else
  {
    for (int z = 0; z < 8; ++z)
    {
      for (int x = 0; x < 8; ++x)
      {
        if (hasSubchunk(x, z))
        {
          int water_index = 9 * z + x, terrain_index = 17 * z + x;

          if ( _vertices[water_index +  0].position.y < chunk->mVertices[terrain_index +  0].y
            && _vertices[water_index +  1].position.y < chunk->mVertices[terrain_index +  1].y
            && _vertices[water_index +  9].position.y < chunk->mVertices[terrain_index + 17].y
            && _vertices[water_index + 10].position.y < chunk->mVertices[terrain_index + 18].y
            )
          {
            setSubchunk(x, z, false);
          }
        }
      }
    }
  }

  update_min_max();
}

void liquid_layer::update_opacity(MapChunk* chunk, float factor)
{
  for (int z = 0; z < 9; ++z)
  {
    for (int x = 0; x < 9; ++x)
    {
      update_vertex_opacity(x, z, chunk, factor);
    }
  }
}

void liquid_layer::update_underground_vertices_depth(MapChunk* chunk)
{
  // set depth = 0 to liquid verts under ground. This is for LODs.
  {
    for (int z = 0; z < 9; ++z)
    {
      for (int x = 0; x < 9; ++x)
      {
        float diff = _vertices[z * 9 + x].position.y - chunk->mVertices[z * 17 + x].y;

        if (diff < 0.f)
        {
          _vertices[z * 9 + x].depth = 0.f;
        }
        else
        {
          if (x < 8 && z < 8 && !hasSubchunk(x, z))
          {
            _vertices[z * 9 + x].depth = 0.f;
            _vertices[z * 9 + x + 1].depth = 0.f;
            _vertices[(z + 1) * 9 + x].depth = 0.f;
            _vertices[(z + 1) * 9 + (x + 1)].depth = 0.f;
          }
        }
      }
    }
  }
}

std::array<liquid_layer::liquid_vertex, 9 * 9>& liquid_layer::getVertices()
{
  return _vertices;
}

float liquid_layer::min() const
{
  return _minimum;
}

float liquid_layer::max() const
{
  return _maximum;
}

int liquid_layer::liquidID() const
{
  return _liquid_id;
}

int liquid_layer::mclq_liquid_type() const
{
  return _mclq_liquid_type;
}

bool liquid_layer::hasSubchunk(int x, int z, int size) const
{
  for (int pz = z; pz < z + size; ++pz)
  {
    for (int px = x; px < x + size; ++px)
    {
      if ((_subchunks >> (pz * 8 + px)) & 1)
      {
        return true;
      }
    }
  }
  return false;
}

void liquid_layer::setSubchunk(int x, int z, bool water)
{
  misc::set_bit(_subchunks, x, z, water);
}

std::uint64_t liquid_layer::getSubchunks()
{
  return _subchunks;
}

bool liquid_layer::empty() const
{
  return !_subchunks;
}

bool liquid_layer::full() const
{
  return _subchunks == std::uint64_t(-1);
}

void liquid_layer::clear()
{
  _subchunks = std::uint64_t(0);
}

void liquid_layer::paintLiquid( glm::vec3 const& cursor_pos
                              , float radius
                              , bool add
                              , math::radians const& angle
                              , math::radians const& orientation
                              , bool lock
                              , glm::vec3 const& origin
                              , bool override_height
                              , MapChunk* chunk
                              , float opacity_factor
                              )
{
  glm::vec3 ref ( lock
                      ? origin
                      : glm::vec3 (cursor_pos.x, cursor_pos.y + 1.0f, cursor_pos.z)
                      );

  int id = 0;

  for (int z = 0; z < 8; ++z)
  {
    for (int x = 0; x < 8; ++x)
    {
      if (misc::getShortestDist(cursor_pos, _vertices[id].position, UNITSIZE) <= radius)
      {
        if (add)
        {
          for (int index : {id, id + 1, id + 9, id + 10})
          {
            bool no_subchunk = !hasSubchunk(x, z);
            bool in_range = misc::dist(cursor_pos, _vertices[index].position) <= radius;

            if (no_subchunk || (in_range && override_height))
            {
              _vertices[index].position.y = misc::angledHeight(ref, _vertices[index].position, angle, orientation);
            }
            if (no_subchunk || in_range)
            {
              update_vertex_opacity(index % 9, index / 9, chunk, opacity_factor);
            }
          }
        }
        setSubchunk(x, z, add);
      }

      id++;
    }
    // to go to the next row of subchunks
    id++;
  }

  update_min_max();
}

// ---- per-vertex height brushes ------------------------------------------------------------
//
// THE SEAM ARGUMENT, which is the whole reason these brushes exist.
//
// A liquid vertex on a chunk border is stored twice, once in each chunk, and a vertex on a
// tile border up to four times. A brush that moves one copy and not the other leaves a visible
// tear, which is the "waterfall between two water heights" the tool was asked to remove. Two
// things have to hold for that not to happen.
//
// (1) Every owner of a shared vertex must be visited. World::for_all_chunks_in_range selects
//     chunks with misc::getShortestDist(pos, chunk_box, CHUNKSIZE) <= radius, and that helper
//     clamps the query point into the CLOSED box, so it returns the distance from the brush
//     centre to the nearest point of the chunk. A shared vertex lies in the closed box of both
//     of its owners, therefore dist(pos, box) <= dist(pos, vertex) for both; if the vertex is
//     inside the brush, both owners are selected. MapIndex::tiles_in_range applies the same
//     test at TILESIZE, so the argument repeats one level up for tile borders. The single
//     exception is a neighbouring tile that is not loaded, exactly as for the terrain brushes.
//
// (2) The value written must be the same in every copy. This is where the stored positions are
//     NOT usable. A chunk's base x is xbase_tile + CHUNKSIZE * chunk_index and a vertex x is
//     that plus UNITSIZE * column; 8 * UNITSIZE == CHUNKSIZE bit-exactly (both are exact
//     power-of-two scalings of TILESIZE = 533.33333f), but the two additions are rounded
//     differently on each side. Evaluating every chunk base of a 64x64 map in float32 found
//     341 of the 1024 chunk column borders where the two owners' stored x differ, by up to
//     0.00390625 yards, which is one float ULP at x = 32900. That is small, but it is enough
//     to put a vertex inside the radius for one owner and outside it for the other, and with a
//     Flat falloff the vertex at the rim still receives the full brush delta - a one-vertex
//     tear of the whole stroke strength.
//
//     So the brushes ignore the stored x/z and rebuild the position from an integer. A chunk
//     base is mathematically (tile_index * 128 + chunk_index * 8) * UNITSIZE, so
//     lround(base / UNITSIZE) recovers that integer; checked against all 1024 chunk columns of
//     a 64x64 map, it recovers it correctly 1024 times out of 1024. Column 8 of chunk k and
//     column 0 of chunk k+1 then both come out as the integer k * 8 + 8, and one integer
//     scaled by one float constant is one float, identical on both sides by construction.
//     Measured over all 9216 vertex columns of a 64x64 map, the canonical position differs
//     from the stored one by at most 0.00390625 yards, i.e. 0.094% of the 4.1667-yard grid
//     spacing, so the brush lands where the user sees the vertex.
//
// What each brush then does to a tear that was already in the data:
//   - changeHeight adds the same delta to both copies, so an existing gap is preserved, never
//     widened. It cannot create one.
//   - flattenHeight mixes both copies towards the same plane value, so the gap is multiplied by
//     (1 - weight) on every application. The weight is at most 1 - 0.5^(dt x strength) and the
//     falloff curve only reduces it further, so it never reaches 1 and the gap decays
//     geometrically rather than vanishing in one tick.
//   - the smooth pass computes one target per world position (the sampler resolves a position
//     to exactly one chunk) and mixes both copies towards it, so the gap decays the same way.
// World::weldLiquidSeams closes a gap outright and is what the panel's "Weld chunk seams"
// checkbox drives after every stroke.

int liquid_layer::gridIndex (float world_coord)
{
  return static_cast<int> (std::lround (world_coord / UNITSIZE));
}

float liquid_layer::gridCoord (int index)
{
  return UNITSIZE * static_cast<float> (index);
}

namespace
{
  int grid_index (float world_coord)
  {
    return liquid_layer::gridIndex (world_coord);
  }

  float grid_coord (int index)
  {
    return liquid_layer::gridCoord (index);
  }

  // The three blend curves MapChunk::flattenTerrain and MapChunk::blurTerrain use, kept
  // identical so a liquid flatten feels like a terrain flatten. eFlattenType_Flat, _Linear and
  // _Smooth are 0, 1 and 2, the same three values as eTerrainType_Flat, _Linear and _Smooth,
  // which is why the panel can drive both enums from one three-way falloff control.
  float flatten_weight (int brush_type, float remain, float dist, float radius)
  {
    switch (brush_type)
    {
      case eFlattenType_Flat:   return remain;
      case eFlattenType_Linear: return remain * (1.f - dist / radius);
      case eFlattenType_Smooth: return std::pow (remain, 1.f + dist / radius);
      default:                  return 0.f;
    }
  }
}

int liquid_layer::gridOriginX() const
{
  return grid_index (pos.x);
}

int liquid_layer::gridOriginZ() const
{
  return grid_index (pos.z);
}

bool liquid_layer::hasVertexData (int x, int z) const
{
  for (int sz (std::max (0, z - 1)); sz <= std::min (7, z); ++sz)
  {
    for (int sx (std::max (0, x - 1)); sx <= std::min (7, x); ++sx)
    {
      if (hasSubchunk (sx, sz))
      {
        return true;
      }
    }
  }

  return false;
}

bool liquid_layer::vertexHeight (int x, int z, float& height) const
{
  if (x < 0 || x > 8 || z < 0 || z > 8 || !hasVertexData (x, z))
  {
    return false;
  }

  height = _vertices[z * 9 + x].position.y;
  return true;
}

void liquid_layer::setVertexHeight (int x, int z, float height, MapChunk* terrain_chunk, float opacity_factor)
{
  if (x < 0 || x > 8 || z < 0 || z > 8)
  {
    return;
  }

  _vertices[z * 9 + x].position.y = height;
  update_vertex_opacity (x, z, terrain_chunk, opacity_factor);
}

void liquid_layer::updateMinMax()
{
  update_min_max();
}

bool liquid_layer::changeHeight ( glm::vec3 const& pos_
                                , float change
                                , float radius
                                , float inner_radius
                                , int brush_type
                                , MapChunk* terrain_chunk
                                , float opacity_factor
                                )
{
  bool changed (false);

  int const origin_x (gridOriginX());
  int const origin_z (gridOriginZ());

  for (int z (0); z < 9; ++z)
  {
    for (int x (0); x < 9; ++x)
    {
      if (!hasVertexData (x, z))
      {
        continue;
      }

      int const index (z * 9 + x);

      // Canonical position, not _vertices[index].position - see the seam argument above. The y
      // component is irrelevant to changeTerrainProcessVertex, which is a 2-D XZ falloff.
      glm::vec3 const vertex ( grid_coord (origin_x + x)
                             , _vertices[index].position.y
                             , grid_coord (origin_z + z)
                             );

      float dt (change);

      // MapChunk::changeTerrainProcessVertex reads nothing from the chunk it is called on; it
      // is the terrain falloff evaluator in free-function form. Calling it rather than copying
      // the curves is what keeps a liquid raise and a terrain raise feeling like one tool.
      if (terrain_chunk->changeTerrainProcessVertex (pos_, vertex, dt, radius, inner_radius, brush_type))
      {
        _vertices[index].position.y += dt;
        update_vertex_opacity (x, z, terrain_chunk, opacity_factor);
        changed = true;
      }
    }
  }

  if (changed)
  {
    update_min_max();
  }

  return changed;
}

bool liquid_layer::flattenHeight ( glm::vec3 const& pos_
                                 , float remain
                                 , float radius
                                 , int brush_type
                                 , flatten_mode const& mode
                                 , glm::vec3 const& origin
                                 , math::radians const& angle
                                 , math::radians const& orientation
                                 , MapChunk* terrain_chunk
                                 , float opacity_factor
                                 )
{
  bool changed (false);

  int const origin_x (gridOriginX());
  int const origin_z (gridOriginZ());

  for (int z (0); z < 9; ++z)
  {
    for (int x (0); x < 9; ++x)
    {
      if (!hasVertexData (x, z))
      {
        continue;
      }

      int const index (z * 9 + x);

      glm::vec3 const vertex ( grid_coord (origin_x + x)
                             , _vertices[index].position.y
                             , grid_coord (origin_z + z)
                             );

      float const dist (misc::dist (vertex, pos_));

      if (dist >= radius)
      {
        continue;
      }

      // The same inclined plane paintLiquid already uses for angled mode, so "flatten to the
      // angled plane" and "paint at the angled plane" agree to the last bit.
      float const ah (misc::angledHeight (origin, vertex, angle, orientation));

      float& y (_vertices[index].position.y);

      if ((!mode.lower && ah < y) || (!mode.raise && ah > y))
      {
        continue;
      }

      if (brush_type == eFlattenType_Origin)
      {
        y = origin.y;
      }
      else
      {
        y = glm::mix (y, ah, flatten_weight (brush_type, remain, dist, radius));
      }

      update_vertex_opacity (x, z, terrain_chunk, opacity_factor);
      changed = true;
    }
  }

  if (changed)
  {
    update_min_max();
  }

  return changed;
}

bool liquid_layer::gatherSmoothedHeights ( glm::vec3 const& pos_
                                         , float remain
                                         , float radius
                                         , int brush_type
                                         , flatten_mode const& mode
                                         , std::function<bool (float, float, int, float&)> const& sampler
                                         , std::array<float, 9 * 9>& target
                                         , std::array<bool, 9 * 9>& mask
                                         ) const
{
  target.fill (0.f);
  mask.fill (false);

  if (brush_type == eFlattenType_Origin)
  {
    return false;
  }

  bool any (false);

  int const origin_x (gridOriginX());
  int const origin_z (gridOriginZ());

  // MapChunk::blurTerrain makes its kernel as wide as the brush: at radius 40 its Rad is
  // (int)(40 / 4.1667) = 9, and its brick pattern is 37 rows by 19 columns, 703 taps per
  // vertex. Liquid has no inner 8x8 row, only the 9x9 outer grid, so the
  // kernel is a plain square on that grid, and it is capped at 4 cells because past that the
  // extra taps only slow the stroke down: a tent kernel gives the outermost ring a weight of
  // 1 - 4/4 = 0, so cell 4 contributes nothing and cells beyond it are not reached at all.
  // At the cap this is 9 x 9 = 81 taps per vertex and at most 81 x 81 = 6561 per chunk-layer.
  int const kernel (std::clamp (static_cast<int> (radius / UNITSIZE), 1, 4));
  float const kernel_extent (grid_coord (kernel));

  for (int z (0); z < 9; ++z)
  {
    for (int x (0); x < 9; ++x)
    {
      if (!hasVertexData (x, z))
      {
        continue;
      }

      int const index (z * 9 + x);

      float const vertex_x (grid_coord (origin_x + x));
      float const vertex_z (grid_coord (origin_z + z));

      float const dist (misc::dist (vertex_x, vertex_z, pos_.x, pos_.z));

      if (dist >= radius)
      {
        continue;
      }

      float total_height (0.f);
      float total_weight (0.f);

      for (int kz (-kernel); kz <= kernel; ++kz)
      {
        for (int kx (-kernel); kx <= kernel; ++kx)
        {
          float const tap_x (grid_coord (origin_x + x + kx));
          float const tap_z (grid_coord (origin_z + z + kz));
          float const tap_dist (misc::dist (tap_x, tap_z, vertex_x, vertex_z));

          if (tap_dist >= kernel_extent)
          {
            continue;
          }

          float height;

          if (sampler (tap_x, tap_z, _liquid_id, height))
          {
            float const weight (1.f - tap_dist / kernel_extent);
            total_height += weight * height;
            total_weight += weight;
          }
        }
      }

      if (total_weight <= 0.f)
      {
        continue;
      }

      float const smoothed (total_height / total_weight);
      float const y (_vertices[index].position.y);

      if ((smoothed > y && !mode.raise) || (smoothed < y && !mode.lower))
      {
        continue;
      }

      target[index] = glm::mix (y, smoothed, flatten_weight (brush_type, remain, dist, radius));
      mask[index] = true;
      any = true;
    }
  }

  return any;
}

void liquid_layer::applyHeights ( std::array<float, 9 * 9> const& target
                                , std::array<bool, 9 * 9> const& mask
                                , MapChunk* terrain_chunk
                                , float opacity_factor
                                )
{
  bool changed (false);

  for (int z (0); z < 9; ++z)
  {
    for (int x (0); x < 9; ++x)
    {
      int const index (z * 9 + x);

      if (!mask[index])
      {
        continue;
      }

      _vertices[index].position.y = target[index];
      update_vertex_opacity (x, z, terrain_chunk, opacity_factor);
      changed = true;
    }
  }

  if (changed)
  {
    update_min_max();
  }
}

void liquid_layer::update_min_max()
{
  _minimum = std::numeric_limits<float>::max();
  _maximum = std::numeric_limits<float>::lowest();
  int x = 0, z = 0;

  for (liquid_vertex& v : _vertices)
  {
    if (hasSubchunk(std::min(x, 7), std::min(z, 7)))
    {
      _maximum = std::max(_maximum, v.position.y);
      _minimum = std::min(_minimum, v.position.y);
    }

    if (++x == 9)
    {
      z++;
      x = 0;
    }
  }

  // lvf = 2 means the liquid height is 0, switch to lvf 0 when that's not the case
  if (_liquid_vertex_format == LVF_DEPTH && (!misc::float_equals(0.f, _minimum) || !misc::float_equals(0.f, _maximum)))
  {
    _liquid_vertex_format = LVF_HEIGHT_DEPTH;
  }
  // use lvf 2 when possible to save space
  else if (_liquid_vertex_format == LVF_HEIGHT_DEPTH && misc::float_equals(0.f, _minimum) && misc::float_equals(0.f, _maximum))
  {
    _liquid_vertex_format = LVF_DEPTH;
  }

  _fatigue_enabled = check_fatigue();
  // recalc all atributes instead?
  // _chunk->update_layers();
}

void liquid_layer::copy_subchunk_height(int x, int z, liquid_layer const& from)
{
  int id = 9 * z + x;

  for (int index : {id, id + 1, id + 9, id + 10})
  {
    _vertices[index].position.y = from._vertices[index].position.y;
  }

  setSubchunk(x, z, true);
}

ChunkWater* liquid_layer::getChunk()
{
  return _chunk;
}

bool liquid_layer::has_fatigue() const
{
  return _fatigue_enabled;
}

void liquid_layer::update_vertex_opacity(int x, int z, MapChunk* chunk, float factor)
{
  const int  index = z * 9 + x;
  float diff = _vertices[index].position.y - chunk->mVertices[z * 17 + x].y;
  _vertices[z * 9 + x].depth = diff < 0.0f ? 0.0f : (std::min(1.0f, std::max(0.0f, (diff + 1.0f) * factor)));
}

int liquid_layer::get_lod_level(glm::vec3 const& camera_pos) const
{
  glm::vec3 const& center_vertex (_vertices[5 * 9 + 4].position);
  // this doesn't look like it's using the right length function...
  // auto const dist ((center_vertex - camera_pos).length());
  float const dist = misc::dist(center_vertex, camera_pos);

  return dist < 1000.f ? 0
       : dist < 2000.f ? 1
       : dist < 4000.f ? 2
       : 3;
}
// if ocean and all subchunks are at max depth
bool liquid_layer::check_fatigue() const
{
    // only oceans have fatigue
    if (_liquid_type != liquid_basic_types_ocean)
    {
        return false;
    }

    for (int z = 0; z < 8; ++z)
    {
        for (int x = 0; x < 8; ++x)
        {
            if (!(hasSubchunk(x, z) && subchunk_at_max_depth(x, z)))
            {
                return false;
            }
        }
    }

    return true;
}

mclq liquid_layer::to_mclq(MH2O_Attributes& attributes) const
{
  mclq mclq_data;

  mclq_data.min_height = _minimum;
  mclq_data.max_height = _maximum;

  for (int i = 0; i < 8 * 8; ++i)
  {
    if (hasSubchunk(i % 8, i / 8))
    {
      mclq_data.tiles[i].liquid_type = _mclq_liquid_type & 0x7;
      mclq_data.tiles[i].dont_render = 0;
      mclq_data.tiles[i].fishable = (attributes.fishable >> i) & 1;
      mclq_data.tiles[i].fatigue = (attributes.fatigue >> i) & 1;
    }
    else
    {
      mclq_data.tiles[i].liquid_type = 7;
      mclq_data.tiles[i].dont_render = 1;
      mclq_data.tiles[i].fishable = 0;
      mclq_data.tiles[i].fatigue = 0;
    }
  }

  for (int i = 0; i < 9 * 9; ++i)
  {
    mclq_data.vertices[i].height = _vertices[i].position.y;

    // magma and slime
    if (_liquid_type == 2 || _liquid_type == 3)
    {
      mclq_data.vertices[i].magma.x = static_cast<std::uint16_t>(std::min(_vertices[i].uv.x * 255.f, 65535.f));
      mclq_data.vertices[i].magma.y = static_cast<std::uint16_t>(std::min(_vertices[i].uv.y * 255.f, 65535.f));
    }
    else
    {
      mclq_data.vertices[i].water.depth = static_cast<std::uint8_t>(std::clamp(_vertices[i].depth * 255.f, 0.f, 255.f));
    }
  }

  return mclq_data;
}

int liquid_layer::mclq_flag_ordering() const
{
  switch (_mclq_liquid_type)
  {
  case 6: return 2;  // lava
  case 3: return 3;  // slime
  case 1: return 1;  // ocean
  default: return 0; // river

  }
}

void liquid_layer::update_attributes(MH2O_Attributes& attributes)
{
    if (check_fatigue())
    {
        attributes.fishable = 0xFFFFFFFFFFFFFFFF;
        attributes.fatigue = 0xFFFFFFFFFFFFFFFF;

        _fatigue_enabled = true;
    }
    else
    {
        _fatigue_enabled = false;
        for (int z = 0; z < 8; ++z)
        {
            for (int x = 0; x < 8; ++x)
            {
                if (hasSubchunk(x, z))
                {
                    // todo : find out when fishable isn't set. maybe lava/slime or very shallow water ?
                    // Most likely when subchunk is entirely above terrain.
                    misc::set_bit(attributes.fishable, x, z, true);

                    // only oceans have fatigue
                    // warning: not used by TrinityCore
                    if (_liquid_type == liquid_basic_types_ocean && subchunk_at_max_depth(x, z))
                    {
                        misc::set_bit(attributes.fatigue, x, z, true);
                    }
                }
            }
        }
    }
}

bool liquid_layer::subchunk_at_max_depth(int x, int z) const
{
    for (int id_z = z; id_z <= z + 1; ++id_z)
    {
        for (int id_x = x; id_x <= x + 1; ++id_x)
        {
            if (_vertices[id_x + 9 * id_z].depth < 1.f)
            {
                return false;
            }
        }
    }

    return true;
}

liquid_layer::liquid_vertex::liquid_vertex(glm::vec3 const& pos, glm::vec2 const& uv, float depth) : position(pos), uv(uv), depth(depth) {}
