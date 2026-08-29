// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/terrain/TerrainMask.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace
{
  // Saturating helpers. Every combinator goes through one of these rather than casting an int back
  // to a byte, because the cast wraps: 200 + 100 narrows to 44, and a user who nudged an Add layer
  // one step too far would watch a fully masked region become almost fully masked OUT. That is the
  // same failure the strength clamp in TerrainRuleStore::load exists to prevent, and it is worth
  // preventing the same way here.
  std::uint8_t saturate(int value)
  {
    return static_cast<std::uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
  }

  int clampInt(int value, int low, int high)
  {
    return value < low ? low : (value > high ? high : value);
  }

  float clampFloat(float value, float low, float high)
  {
    // Written so a NaN argument leaves through the `low` branch rather than propagating. A NaN
    // opacity or amount reaches here from a settings file or a spin box that was mid-edit, and a
    // NaN multiplied into an alphamap delta silently blanks a chunk.
    if (!(value > low))
    {
      return low;
    }

    return value > high ? high : value;
  }
}

namespace Noggit
{
  // --- MaskChunkAddress ---

  bool MaskChunkAddress::valid() const
  {
    return tile_x >= 0 && tile_x < MASK_MAP_TILE_SIDE
        && tile_z >= 0 && tile_z < MASK_MAP_TILE_SIDE
        && chunk_x >= 0 && chunk_x < MASK_TILE_CHUNK_SIDE
        && chunk_z >= 0 && chunk_z < MASK_TILE_CHUNK_SIDE;
  }

  std::uint32_t MaskChunkAddress::packed() const
  {
    // Tile first, then chunk, so ascending packed() is ascending tile-major order. storedChunks()
    // sorts on this and gets tile locality for free, which is what makes a saved mask compress and
    // what makes releaseTile's scan touch one contiguous key range.
    return (static_cast<std::uint32_t>(tile_x & 63) << 14)
         | (static_cast<std::uint32_t>(tile_z & 63) << 8)
         | (static_cast<std::uint32_t>(chunk_x & 15) << 4)
         | (static_cast<std::uint32_t>(chunk_z & 15));
  }

  MaskChunkAddress MaskChunkAddress::fromPacked(std::uint32_t packed)
  {
    MaskChunkAddress address;
    address.tile_x = static_cast<int>((packed >> 14) & 63);
    address.tile_z = static_cast<int>((packed >> 8) & 63);
    address.chunk_x = static_cast<int>((packed >> 4) & 15);
    address.chunk_z = static_cast<int>(packed & 15);
    return address;
  }

  MaskChunkAddress MaskChunkAddress::fromWorld(float x, float z)
  {
    MaskChunkAddress address;

    // std::floor, not a truncating cast. They differ for negative coordinates, which occur every
    // time a brush overlaps the western or northern edge of the map, and a truncating cast would
    // fold x = -3.0 into tile 0 rather than reporting it as off the map. The address would be
    // valid() and would clip a brush against the wrong chunk.
    float const tile_x_f = std::floor(x / MASK_TILE_SIZE);
    float const tile_z_f = std::floor(z / MASK_TILE_SIZE);

    // Bounds-tested before the cast rather than after. A non-finite coordinate -- which reaches
    // here from an unprojection that missed the terrain -- has no defined conversion to int at all,
    // so it must not be cast; the comparison chain below is false for NaN and returns an address
    // that fails valid().
    if (!(tile_x_f >= 0.0f) || !(tile_x_f < static_cast<float>(MASK_MAP_TILE_SIDE))
     || !(tile_z_f >= 0.0f) || !(tile_z_f < static_cast<float>(MASK_MAP_TILE_SIDE)))
    {
      address.tile_x = -1;
      return address;
    }

    address.tile_x = static_cast<int>(tile_x_f);
    address.tile_z = static_cast<int>(tile_z_f);

    float const in_tile_x = x - tile_x_f * MASK_TILE_SIZE;
    float const in_tile_z = z - tile_z_f * MASK_TILE_SIZE;

    // Clamped rather than trusted. TILESIZE is 533.33333f, which is not the exact value the map is
    // laid out on, so x / MASK_TILE_SIZE at the far edge of tile 63 can land a whisker past 16
    // chunks once the subtraction above has lost its low bits.
    address.chunk_x = clampInt(static_cast<int>(in_tile_x / MASK_CHUNK_SIZE), 0, MASK_TILE_CHUNK_SIDE - 1);
    address.chunk_z = clampInt(static_cast<int>(in_tile_z / MASK_CHUNK_SIZE), 0, MASK_TILE_CHUNK_SIDE - 1);

    return address;
  }

  bool operator==(MaskChunkAddress const& lhs, MaskChunkAddress const& rhs)
  {
    return lhs.tile_x == rhs.tile_x && lhs.tile_z == rhs.tile_z
        && lhs.chunk_x == rhs.chunk_x && lhs.chunk_z == rhs.chunk_z;
  }

  bool operator!=(MaskChunkAddress const& lhs, MaskChunkAddress const& rhs)
  {
    return !(lhs == rhs);
  }

  // --- Combinators ---

  std::uint8_t maskCombine(MaskCombine op, std::uint8_t accumulated, std::uint8_t value)
  {
    int const a = accumulated;
    int const b = value;

    switch (op)
    {
      case MaskCombine::Replace:
        return value;
      case MaskCombine::Add:
        return saturate(a + b);
      case MaskCombine::Subtract:
        return saturate(a - b);
      case MaskCombine::Multiply:
        // Rounded, not truncated, and the rounding is what makes Multiply by 255 an exact identity.
        // Truncating gives (254 * 255) / 255 = 253 for a full-strength layer, so a stack of six
        // Multiply layers would fade a mask the user believes is untouched by roughly 2 percent per
        // layer. The +127 is a half-step at this scale.
        return static_cast<std::uint8_t>((a * b + 127) / 255);
      case MaskCombine::Min:
        return static_cast<std::uint8_t>(a < b ? a : b);
      case MaskCombine::Max:
        return static_cast<std::uint8_t>(a > b ? a : b);
    }

    return accumulated;
  }

  char const* maskCombineName(MaskCombine op)
  {
    switch (op)
    {
      case MaskCombine::Replace:  return "replace";
      case MaskCombine::Add:      return "add";
      case MaskCombine::Subtract: return "subtract";
      case MaskCombine::Multiply: return "multiply";
      case MaskCombine::Min:      return "min";
      case MaskCombine::Max:      return "max";
    }

    return "replace";
  }

  MaskCombine maskCombineFromName(char const* name)
  {
    if (!name)
    {
      return MaskCombine::Replace;
    }

    if (std::strcmp(name, "add") == 0)      { return MaskCombine::Add; }
    if (std::strcmp(name, "subtract") == 0) { return MaskCombine::Subtract; }
    if (std::strcmp(name, "multiply") == 0) { return MaskCombine::Multiply; }
    if (std::strcmp(name, "min") == 0)      { return MaskCombine::Min; }
    if (std::strcmp(name, "max") == 0)      { return MaskCombine::Max; }

    return MaskCombine::Replace;
  }

  // --- TerrainMask ---

  TerrainMask::TerrainMask(TerrainMask const& other)
  {
    *this = other;
  }

  TerrainMask& TerrainMask::operator=(TerrainMask const& other)
  {
    if (this == &other)
    {
      return *this;
    }

    _blocks.clear();
    _blocks.reserve(other._blocks.size());

    for (auto const& entry : other._blocks)
    {
      Block copy;
      copy.uniform = entry.second.uniform;

      if (entry.second.texels)
      {
        copy.texels.reset(new std::uint8_t[MASK_CHUNK_TEXELS]);
        std::memcpy(copy.texels.get(), entry.second.texels.get(), MASK_CHUNK_TEXELS);
      }

      _blocks.emplace(entry.first, std::move(copy));
    }

    _inverted = other._inverted;
    _clamp_low = other._clamp_low;
    _clamp_high = other._clamp_high;

    return *this;
  }

  std::uint8_t* TerrainMask::densify(Block& block)
  {
    if (!block.texels)
    {
      block.texels.reset(new std::uint8_t[MASK_CHUNK_TEXELS]);
      std::memset(block.texels.get(), block.uniform, MASK_CHUNK_TEXELS);
    }

    return block.texels.get();
  }

  void TerrainMask::collapse(Block& block)
  {
    if (!block.texels)
    {
      return;
    }

    std::uint8_t const* const texels = block.texels.get();
    std::uint8_t const first = texels[0];

    // memchr for the first byte that differs would need a complement scan; a plain loop with an
    // early exit is what this is. It stops at the first disagreement, so the common case -- a chunk
    // that is genuinely mixed -- costs two comparisons, not 4096.
    for (int i = 1; i < MASK_CHUNK_TEXELS; ++i)
    {
      if (texels[i] != first)
      {
        return;
      }
    }

    block.uniform = first;
    block.texels.reset();
  }

  TerrainMask::Block const* TerrainMask::find(MaskChunkAddress const& address) const
  {
    if (!address.valid())
    {
      return nullptr;
    }

    auto const it = _blocks.find(address.packed());
    return it == _blocks.end() ? nullptr : &it->second;
  }

  std::uint8_t TerrainMask::present(std::uint8_t stored) const
  {
    int value = _inverted ? (MASK_MAX - stored) : stored;

    value = clampInt(value, _clamp_low, _clamp_high);

    return static_cast<std::uint8_t>(value);
  }

  std::uint8_t TerrainMask::texelAt(MaskChunkAddress const& address, int col, int row) const
  {
    if (col < 0 || col >= MASK_CHUNK_SIDE || row < 0 || row >= MASK_CHUNK_SIDE)
    {
      return 0;
    }

    Block const* const block = find(address);

    // An absent chunk reads as the presented form of zero, NOT as a literal zero. On an inverted
    // mask that is 255, which is the entire point: inverting "the cliffs" has to produce
    // "everything that is not the cliffs", including the thousands of chunks the cliff mask never
    // stored. Returning a literal 0 here would make invert() a no-op outside the stored region and
    // the inverted mask would clip everything away.
    if (!block)
    {
      return address.valid() ? present(0) : 0;
    }

    return present(block->texels ? block->texels[static_cast<std::size_t>(col) + MASK_CHUNK_SIDE * static_cast<std::size_t>(row)]
                                 : block->uniform);
  }

  float TerrainMask::valueAt(float x, float z) const
  {
    // Texel centres sit at half-integer positions in global texel space, so the interpolation
    // parameter is offset by half a texel. Without the offset the field would be shifted by
    // MASK_TEXEL_SIZE / 2 = 0.26 yards against the alphamap it is clipping, which is invisible in
    // isolation and shows up as a mask that does not quite line up with the paint it produced.
    float const gx = x / MASK_TEXEL_SIZE - 0.5f;
    float const gz = z / MASK_TEXEL_SIZE - 0.5f;

    if (!(gx > -1.0e9f) || !(gx < 1.0e9f) || !(gz > -1.0e9f) || !(gz < 1.0e9f))
    {
      // Non-finite input. Fails open at 1.0 rather than closed at 0.0: a mask query that cannot be
      // answered must not silently disable the brush the user is holding down. See the same
      // decision, argued at length, in TerrainMaskQuery.
      return 1.0f;
    }

    float const x0 = std::floor(gx);
    float const z0 = std::floor(gz);
    float const fx = gx - x0;
    float const fz = gz - z0;

    // The four corners are addressed through world coordinates rather than through global texel
    // indices, so the tile and chunk decomposition happens once per corner in one place. It is four
    // hash lookups per query, which is the cost of the bilinear filter and the reason texelAt
    // exists for the texel-aligned texture path.
    auto corner = [this] (float texel_x, float texel_z) -> float
    {
      float const wx = (texel_x + 0.5f) * MASK_TEXEL_SIZE;
      float const wz = (texel_z + 0.5f) * MASK_TEXEL_SIZE;

      MaskChunkAddress const address = MaskChunkAddress::fromWorld(wx, wz);

      if (!address.valid())
      {
        // Off the map. Treated exactly like an absent chunk rather than as a hard zero, so an
        // inverted mask stays inverted across the map edge instead of growing a 255-to-0 seam at
        // x = 0 that no terrain edit could ever have produced.
        return static_cast<float>(present(0)) / static_cast<float>(MASK_MAX);
      }

      float const in_chunk_x = wx - (static_cast<float>(address.tile_x) * MASK_TILE_SIZE
                                   + static_cast<float>(address.chunk_x) * MASK_CHUNK_SIZE);
      float const in_chunk_z = wz - (static_cast<float>(address.tile_z) * MASK_TILE_SIZE
                                   + static_cast<float>(address.chunk_z) * MASK_CHUNK_SIZE);

      int const col = clampInt(static_cast<int>(in_chunk_x / MASK_TEXEL_SIZE), 0, MASK_CHUNK_SIDE - 1);
      int const row = clampInt(static_cast<int>(in_chunk_z / MASK_TEXEL_SIZE), 0, MASK_CHUNK_SIDE - 1);

      return static_cast<float>(texelAt(address, col, row)) / static_cast<float>(MASK_MAX);
    };

    float const v00 = corner(x0, z0);
    float const v10 = corner(x0 + 1.0f, z0);
    float const v01 = corner(x0, z0 + 1.0f);
    float const v11 = corner(x0 + 1.0f, z0 + 1.0f);

    float const top = v00 + (v10 - v00) * fx;
    float const bottom = v01 + (v11 - v01) * fx;

    return top + (bottom - top) * fz;
  }

  void TerrainMask::setTexel(MaskChunkAddress const& address, int col, int row, std::uint8_t value)
  {
    if (!address.valid() || col < 0 || col >= MASK_CHUNK_SIDE || row < 0 || row >= MASK_CHUNK_SIDE)
    {
      return;
    }

    Block& block = _blocks[address.packed()];

    // The cheap exit that keeps repainting free. A stroke dragged back and forth over a region that
    // is already saturated writes the value it finds, and without this test every one of those
    // chunks would materialise 4 KiB to hold 4096 identical bytes.
    if (!block.texels && block.uniform == value)
    {
      return;
    }

    densify(block)[static_cast<std::size_t>(col) + MASK_CHUNK_SIDE * static_cast<std::size_t>(row)] = value;
  }

  void TerrainMask::fillChunk(MaskChunkAddress const& address, std::uint8_t value)
  {
    if (!address.valid())
    {
      return;
    }

    Block& block = _blocks[address.packed()];
    block.uniform = value;
    block.texels.reset();
  }

  void TerrainMask::writeChunk(MaskChunkAddress const& address, std::uint8_t const* texels)
  {
    if (!address.valid() || !texels)
    {
      return;
    }

    Block& block = _blocks[address.packed()];

    std::memcpy(densify(block), texels, MASK_CHUNK_TEXELS);

    collapse(block);
  }

  void TerrainMask::combineChunk( MaskChunkAddress const& address
                                , std::uint8_t const* texels
                                , MaskCombine op
                                , float opacity
                                )
  {
    if (!address.valid() || !texels)
    {
      return;
    }

    float const weight = clampFloat(opacity, 0.0f, 1.0f);

    Block& block = _blocks[address.packed()];
    std::uint8_t* const destination = densify(block);

    for (int i = 0; i < MASK_CHUNK_TEXELS; ++i)
    {
      std::uint8_t const scaled
        = static_cast<std::uint8_t>(static_cast<float>(texels[i]) * weight + 0.5f);

      destination[i] = maskCombine(op, destination[i], scaled);
    }

    collapse(block);
  }

  bool TerrainMask::readChunk(MaskChunkAddress const& address, std::uint8_t* out) const
  {
    if (!address.valid() || !out)
    {
      return false;
    }

    Block const* const block = find(address);

    if (!block)
    {
      std::memset(out, present(0), MASK_CHUNK_TEXELS);
      return true;
    }

    if (!block->texels)
    {
      std::memset(out, present(block->uniform), MASK_CHUNK_TEXELS);
      return true;
    }

    for (int i = 0; i < MASK_CHUNK_TEXELS; ++i)
    {
      out[i] = present(block->texels[i]);
    }

    return true;
  }

  bool TerrainMask::chunkIsDense(MaskChunkAddress const& address) const
  {
    Block const* const block = find(address);
    return block && block->texels;
  }

  std::uint8_t TerrainMask::chunkUniformValue(MaskChunkAddress const& address) const
  {
    Block const* const block = find(address);
    return (block && !block->texels) ? block->uniform : 0;
  }

  void TerrainMask::invert()
  {
    // A flag, not a pass over the data, and that is not an optimisation -- it is the only correct
    // implementation. Inverting the stored texels would leave every ABSENT chunk reading zero, so
    // the complement of a small mask would be another small mask instead of "everything else". The
    // flag inverts absence too. See texelAt.
    _inverted = !_inverted;

    // The clamp window is expressed in presented values, so it survives the flip unchanged. It is
    // reset only by clampTo.
  }

  bool TerrainMask::inverted() const
  {
    return _inverted;
  }

  void TerrainMask::clampTo(std::uint8_t low, std::uint8_t high)
  {
    // An inverted window is folded rather than rejected, because the two spin boxes that produce it
    // can be dragged past each other. Folding gives the one-value window the user is halfway to
    // typing; rejecting would leave the previous window in place and look like the control was
    // broken.
    _clamp_low = low < high ? low : high;
    _clamp_high = low < high ? high : low;
  }

  std::size_t TerrainMask::paintCircle( float x
                                      , float z
                                      , float radius
                                      , float hardness
                                      , float amount
                                      , MaskCombine op
                                      , bool complement
                                      )
  {
    if (!(radius > 0.0f))
    {
      return 0;
    }

    float const hard = clampFloat(hardness, 0.0f, 1.0f);
    float const strength = clampFloat(amount, 0.0f, 1.0f);
    float const inner = radius * hard;

    // Texel index range of the bounding square, in global texel space. Walking texels rather than
    // chunks keeps the falloff computation in one loop; the chunk address is recomputed per texel,
    // which is a division and a hash lookup and is not the expensive part of a brush stroke.
    int const first_col = static_cast<int>(std::floor((x - radius) / MASK_TEXEL_SIZE));
    int const last_col = static_cast<int>(std::floor((x + radius) / MASK_TEXEL_SIZE));
    int const first_row = static_cast<int>(std::floor((z - radius) / MASK_TEXEL_SIZE));
    int const last_row = static_cast<int>(std::floor((z + radius) / MASK_TEXEL_SIZE));

    std::size_t touched = 0;
    std::uint32_t last_packed = 0;
    bool have_last = false;

    for (int row = first_row; row <= last_row; ++row)
    {
      float const wz = (static_cast<float>(row) + 0.5f) * MASK_TEXEL_SIZE;

      for (int col = first_col; col <= last_col; ++col)
      {
        float const wx = (static_cast<float>(col) + 0.5f) * MASK_TEXEL_SIZE;

        float const dx = wx - x;
        float const dz = wz - z;
        float const distance = std::sqrt(dx * dx + dz * dz);

        if (distance > radius)
        {
          continue;
        }

        // Linear feather from the hard edge outward, which is the shape Brush::getValue produces
        // for the texture brush. Reusing it means a mask painted with radius R and hardness H has
        // the same profile as a texture stroke with the same two numbers, so the two gestures agree
        // about what "the edge of the brush" means.
        float falloff = 1.0f;

        if (distance > inner && radius > inner)
        {
          falloff = 1.0f - (distance - inner) / (radius - inner);
        }

        float const weighted = clampFloat(strength * falloff, 0.0f, 1.0f);

        // See the `complement` note on the declaration: an erase stroke needs the rim to tend to
        // 255 so that Min leaves it alone, not to 0 which would make Min erase hardest at the edge.
        std::uint8_t const value = static_cast<std::uint8_t>
          ((complement ? (1.0f - weighted) : weighted) * static_cast<float>(MASK_MAX) + 0.5f);

        MaskChunkAddress const address = MaskChunkAddress::fromWorld(wx, wz);

        if (!address.valid())
        {
          continue;
        }

        std::uint32_t const packed = address.packed();

        if (!have_last || packed != last_packed)
        {
          last_packed = packed;
          have_last = true;
          ++touched;
        }

        int const in_col = static_cast<int>
          (col - (address.tile_x * MASK_TILE_CHUNK_SIDE + address.chunk_x) * MASK_CHUNK_SIDE);
        int const in_row = static_cast<int>
          (row - (address.tile_z * MASK_TILE_CHUNK_SIDE + address.chunk_z) * MASK_CHUNK_SIDE);

        if (in_col < 0 || in_col >= MASK_CHUNK_SIDE || in_row < 0 || in_row >= MASK_CHUNK_SIDE)
        {
          continue;
        }

        Block& block = _blocks[packed];
        std::uint8_t* const destination = densify(block);
        std::size_t const offset
          = static_cast<std::size_t>(in_col) + MASK_CHUNK_SIDE * static_cast<std::size_t>(in_row);

        destination[offset] = maskCombine(op, destination[offset], value);
      }
    }

    return touched;
  }

  void TerrainMask::releaseTile(int tile_x, int tile_z)
  {
    if (tile_x < 0 || tile_x >= MASK_MAP_TILE_SIDE || tile_z < 0 || tile_z >= MASK_MAP_TILE_SIDE)
    {
      return;
    }

    // The 256 keys of one tile are contiguous in packed() space, so they are erased by
    // construction rather than by scanning the map. A scan would be O(total chunks) per tile
    // unload, and tile unload runs on a timer (map_index.cpp:499) while the camera is moving.
    MaskChunkAddress address;
    address.tile_x = tile_x;
    address.tile_z = tile_z;

    for (int chunk_z = 0; chunk_z < MASK_TILE_CHUNK_SIDE; ++chunk_z)
    {
      address.chunk_z = chunk_z;

      for (int chunk_x = 0; chunk_x < MASK_TILE_CHUNK_SIDE; ++chunk_x)
      {
        address.chunk_x = chunk_x;
        _blocks.erase(address.packed());
      }
    }
  }

  void TerrainMask::clear()
  {
    _blocks.clear();
    _inverted = false;
    _clamp_low = 0;
    _clamp_high = MASK_MAX;
  }

  bool TerrainMask::empty() const
  {
    return _blocks.empty();
  }

  std::size_t TerrainMask::chunkCount() const
  {
    return _blocks.size();
  }

  std::size_t TerrainMask::denseChunkCount() const
  {
    std::size_t count = 0;

    for (auto const& entry : _blocks)
    {
      count += entry.second.texels ? 1u : 0u;
    }

    return count;
  }

  std::size_t TerrainMask::uniformChunkCount() const
  {
    return _blocks.size() - denseChunkCount();
  }

  std::size_t TerrainMask::bytes() const
  {
    // The overhead term is the node's value_type plus three pointers: two for the intrusive linkage
    // every standard unordered_map node carries and one amortised bucket slot at the default
    // max_load_factor of 1. It is a bound rather than an exact figure -- the standard does not
    // specify the node layout and MSVC's list-based map differs from libstdc++'s -- and it is
    // stated here rather than hidden so the number the UI prints can be read as "payload plus
    // bookkeeping" instead of as a measurement of the allocator.
    constexpr std::size_t entry_overhead
      = sizeof(std::pair<std::uint32_t const, Block>) + 3 * sizeof(void*);

    return _blocks.size() * entry_overhead
         + denseChunkCount() * static_cast<std::size_t>(MASK_CHUNK_TEXELS);
  }

  std::vector<MaskChunkAddress> TerrainMask::storedChunks() const
  {
    std::vector<std::uint32_t> keys;
    keys.reserve(_blocks.size());

    for (auto const& entry : _blocks)
    {
      keys.push_back(entry.first);
    }

    // Sorted because an unordered_map's iteration order is an implementation detail that varies
    // with insertion history. Without this a mask saved twice from the same field would produce two
    // different files, which turns every save into a spurious diff in a project under version
    // control.
    std::sort(keys.begin(), keys.end());

    std::vector<MaskChunkAddress> addresses;
    addresses.reserve(keys.size());

    for (std::uint32_t key : keys)
    {
      addresses.push_back(MaskChunkAddress::fromPacked(key));
    }

    return addresses;
  }
}
