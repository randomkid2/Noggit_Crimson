// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_TERRAINMASK_HPP
#define NOGGIT_TERRAINMASK_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

// A MASK is a scalar field over the map that decides WHERE an operation applies, and nothing else.
//
// It is an editor-side concept only. Nothing here is ever written into an ADT, an alphamap or any
// other client file: a mask multiplies the strength of some other edit and then leaves no trace, so
// it carries exactly zero risk of producing a file the client will not load. That property is the
// reason masks can be added freely where a new chunk sub-chunk could not, and every design decision
// below is allowed to lean on it.
//
// This header is PURE STL on purpose, the same arrangement TerrainRules.hpp uses and for the same
// reason: everything with an out-of-line definition lives in TerrainMask.cpp and depends on no Qt,
// no glm, no OpenGL and no MapChunk, so tests/TerrainMaskTests.cpp links it on a bare machine. The
// geometry constants below are RESTATED rather than included from MapHeaders.h, because that header
// pulls in <glm/vec3.hpp> and the standalone test target has no glm include directory.
// TerrainMaskTests.cpp asserts the restated values against their derivations so the two cannot
// drift apart silently.
namespace Noggit
{
  // --- Geometry. Restated from MapHeaders.h:58-62; see the note above. ---

  inline constexpr float MASK_TILE_SIZE = 533.33333f;
  inline constexpr float MASK_CHUNK_SIZE = MASK_TILE_SIZE / 16.0f;

  // 0.52083 yards. The alphamap texel, and therefore the finest thing any masked operation can
  // resolve. See MASK_CHUNK_SIDE for why the mask matches it exactly.
  inline constexpr float MASK_TEXEL_SIZE = MASK_CHUNK_SIZE / 64.0f;

  // A mask block is 64x64 texels covering one chunk -- EXACTLY the layout of MCAL.
  //
  // This is the central sizing decision and it is worth stating why it is not something coarser.
  // The finest consumer of a mask is the texture brush, whose inner loop walks a 64x64 grid per
  // chunk (texture_set.cpp:902-910). A mask stored more coarsely than that would clip the brush in
  // visible square steps -- a mask at the 8x8 unit grid would step in 4.17-yard blocks, which is
  // roughly the width of a footpath and is obvious on any shoreline. Matching MCAL means the mask
  // edge is exactly as sharp as the paint it is clipping and never sharper, which is the only
  // resolution at which the clip is invisible as a clip.
  inline constexpr int MASK_CHUNK_SIDE = 64;
  inline constexpr int MASK_CHUNK_TEXELS = MASK_CHUNK_SIDE * MASK_CHUNK_SIDE;

  inline constexpr int MASK_TILE_CHUNK_SIDE = 16;
  inline constexpr int MASK_MAP_TILE_SIDE = 64;

  // Values are stored as bytes, not floats, and the public API is float.
  //
  // 8 bits is not a compromise here, it is the exact precision of the thing being clipped: an
  // alphamap texel is a byte (TerrainRules.hpp:41), so a mask with more than 256 levels cannot
  // express a distinction the texture path could act on. It also makes the field a quarter of the
  // size a float field would be, which at these dimensions is the difference between 4 GiB and
  // 16 GiB for a dense continent -- see the memory note on TerrainMask.
  inline constexpr std::uint8_t MASK_MAX = 255;

  // --- Addressing ---

  // One chunk of the field: which tile, and which chunk inside it.
  //
  // Chunk granularity rather than tile granularity is what makes the sparse storage pay. A derived
  // mask such as "slopes above 40 degrees" is uniformly zero over every chunk of flat ground, and a
  // uniform chunk costs no payload at all (see TerrainMask). Storing whole tiles would force a
  // 1 MiB allocation for a tile containing one cliff.
  struct MaskChunkAddress
  {
    int tile_x = 0;
    int tile_z = 0;
    int chunk_x = 0;
    int chunk_z = 0;

    // Tile inside [0, 64), chunk inside [0, 16). An out-of-range address is not an error at the
    // call site -- a brush near the edge of the map legitimately produces one -- so every entry
    // point below tests this and does nothing rather than asserting.
    bool valid() const;

    // 20 bits: 6 for each tile axis, 4 for each chunk axis. The key type of the storage map.
    // Ordered so that packed() sorts by tile then chunk, which is what makes serialisation output
    // deterministic without a separate sort key.
    std::uint32_t packed() const;
    static MaskChunkAddress fromPacked(std::uint32_t packed);

    // The chunk containing a world position. Editor space, where tile_x is floor(x / TILESIZE)
    // (TileIndex.cpp:11-12) -- NOT server space, which is ZEROPOINT minus the editor coordinate
    // and has the axes swapped (SceneObject.cpp:196).
    //
    // Returns an invalid address for a position off the map; callers test valid().
    static MaskChunkAddress fromWorld(float x, float z);
  };

  bool operator==(MaskChunkAddress const& lhs, MaskChunkAddress const& rhs);
  bool operator!=(MaskChunkAddress const& lhs, MaskChunkAddress const& rhs);

  // --- Combinators ---

  // How a new value folds into an accumulated one. This is what turns a list of filters into a
  // STACK rather than a single test, and it is the whole reason the feature is more than a slope
  // threshold: "steep AND low" is Multiply, "steep OR near water" is Max, "steep but not on the
  // road" is Subtract.
  //
  // Min and Max are here alongside Multiply and Add because they are the ones that behave like
  // logical AND and OR -- Multiply darkens a soft edge every time it is applied and Add saturates,
  // whereas Min and Max leave a soft edge exactly as soft as it was. A user combining four filters
  // with Multiply and wondering why the result is nearly black is the predictable failure this set
  // is shaped to avoid.
  enum class MaskCombine
  {
    Replace,
    Add,
    Subtract,
    Multiply,
    Min,
    Max
  };

  // Every operation saturates at 0 and MASK_MAX rather than wrapping. Wrapping would turn
  // "subtract a little too much" into "fully masked in", which is the same class of silent
  // catastrophe as the strength clamp in TerrainRuleStore::load.
  std::uint8_t maskCombine(MaskCombine op, std::uint8_t accumulated, std::uint8_t value);

  // Stable spellings for persistence and for the UI. Unknown text reads back as Replace, which is
  // the identity-losing but non-destructive choice: a stack whose combinator failed to parse
  // produces the last filter's output rather than an empty mask.
  char const* maskCombineName(MaskCombine op);
  MaskCombine maskCombineFromName(char const* name);

  // --- The field ---

  // A sparse scalar field, stored as 64x64-texel blocks keyed by chunk.
  //
  // MEMORY, computed rather than estimated. One texel is one byte and covers
  // MASK_TEXEL_SIZE^2 = 0.2713 square yards:
  //
  //   dense chunk block   64 x 64            =       4096 B  =   4.000 KiB
  //   dense tile          256 chunks         =  1'048'576 B  =   1.000 MiB
  //   dense 64x64 map     4096 tiles         =        4 GiB
  //
  // The last number is why the storage is sparse and why uniform chunks are collapsed. A dense
  // continent is not affordable and never has to be: Noggit keeps only the tiles near the camera
  // resident, and MapIndex unloads anything beyond `unload_dist` tiles, default 5
  // (map_index.cpp:82). The resident set is therefore bounded by the integer disc of radius 5,
  // which contains 81 tiles, so a mask that is dense over every resident tile costs
  // 81 MiB and cannot grow past it as long as releaseTile is wired to tile unload.
  //
  // In practice it is far less, because of the two-state block below. A block is either UNIFORM --
  // a single byte, no payload, no allocation -- or DENSE. Derived masks are overwhelmingly uniform:
  // "slope above 40 degrees" is uniform zero over every chunk of flat ground and uniform 255 over
  // the interior of every cliff face, and only the chunks straddling the threshold pay. An absent
  // key is uniform zero, so a freshly created mask costs nothing at all.
  class TerrainMask
  {
  public:
    TerrainMask() = default;

    // COPY IS A DEEP COPY, and it has to be written out because the dense payload is held by
    // unique_ptr. The implicitly declared copy constructor is not merely absent here -- it is
    // ill-formed the moment anything instantiates it, which the compiler reports as a deleted
    // function somewhere inside unordered_map rather than as anything to do with this class.
    //
    // The alternative -- holding each block in a std::vector<std::uint8_t>, which copies itself --
    // was rejected on the memory arithmetic this whole class is shaped by: a vector adds two words
    // of size and capacity to EVERY block including the uniform ones, which are the majority in any
    // derived mask, and doubles the footprint of a block that stores nothing. Fifteen lines of
    // explicit copy is the cheaper trade.
    //
    // Masks genuinely need to be copyable: TerrainMaskStore keeps them in a vector, and duplicating
    // a mask to try a variation on it is an obvious thing for a user to ask for.
    TerrainMask(TerrainMask const& other);
    TerrainMask& operator=(TerrainMask const& other);

    // Moves are the cheap path and are what the store's vector uses when it reallocates.
    TerrainMask(TerrainMask&&) = default;
    TerrainMask& operator=(TerrainMask&&) = default;

    ~TerrainMask() = default;

    // Bilinear sample at an arbitrary world position, in [0, 1].
    //
    // Off the map reads exactly like an absent chunk rather than as a hard zero, so an inverted
    // mask stays inverted across the map edge instead of growing a seam at x = 0 that no terrain
    // edit could have produced. A non-finite coordinate returns 1.0 -- fully unmasked -- because a
    // query that cannot be answered must not be the thing that silently disables a live brush.
    //
    // Bilinear rather than nearest, and this matters for exactly one consumer: terrain vertices sit
    // UNITSIZE = 4.1667 yards apart, eight times the mask texel, so a terrain brush point-sampling
    // the field would take one texel's value as the verdict for a 17-square-yard neighbourhood and
    // the mask edge would land on whichever texel happened to sit under a vertex. Interpolating
    // makes the edge move smoothly as the vertex grid slides past it. The texture path does not use
    // this -- it is texel-aligned already and calls texelAt, which is exact and cheaper.
    float valueAt(float x, float z) const;

    // Exact value of one texel. col is the X axis, row is the Z axis, matching the `i + 64 * j`
    // indexing the alphamaps use (texture_set.cpp:908). Out-of-range returns 0.
    std::uint8_t texelAt(MaskChunkAddress const& address, int col, int row) const;

    // Writes one texel, materialising the block if it was uniform. Writing a texel equal to the
    // block's uniform value does NOT materialise it, so repainting a value that is already there
    // costs no memory.
    void setTexel(MaskChunkAddress const& address, int col, int row, std::uint8_t value);

    // Sets a whole chunk to one value. Releases any dense payload the chunk had; a fill is the
    // cheapest possible state and there is no reason to keep 4 KiB describing it.
    void fillChunk(MaskChunkAddress const& address, std::uint8_t value);

    // Replaces a chunk from MASK_CHUNK_TEXELS bytes, collapsing to a uniform block when every texel
    // agrees. The collapse is what keeps a derived mask small, so it is done on write rather than
    // in a separate pass -- a pass would have to visit chunks that are already uniform.
    void writeChunk(MaskChunkAddress const& address, std::uint8_t const* texels);

    // Folds MASK_CHUNK_TEXELS bytes into a chunk with `op`, scaled by `opacity` in [0, 1].
    //
    // opacity is applied to the incoming VALUE, not to the result, so Multiply at opacity 0 is the
    // identity and Multiply at opacity 0.5 is a half-strength darkening rather than a half-strength
    // blend toward black. That is the behaviour a layer opacity slider is expected to have.
    void combineChunk( MaskChunkAddress const& address
                     , std::uint8_t const* texels
                     , MaskCombine op
                     , float opacity
                     );

    // Expands a chunk into MASK_CHUNK_TEXELS bytes, uniform or dense. Returns false and writes
    // nothing for an invalid address or a null destination. A chunk that is not stored expands to
    // all zeroes and returns true -- absent means zero, not unknown.
    bool readChunk(MaskChunkAddress const& address, std::uint8_t* out) const;

    // True when the chunk holds a dense payload. Exposed for the tests and the memory readout,
    // which are the only things that should care.
    bool chunkIsDense(MaskChunkAddress const& address) const;
    // The value of a uniform chunk, or 0 when it is dense or absent.
    std::uint8_t chunkUniformValue(MaskChunkAddress const& address) const;

    // --- Whole-field operators ---

    // 255 - v everywhere the field is stored, AND everywhere it is not: inverting a mask has to
    // turn the implicit zero outside the stored chunks into 255, which cannot be represented
    // sparsely. Rather than materialise the whole map, an inverted field flips its interpretation
    // of absence. `inverted()` reports that state and valueAt accounts for it.
    void invert();
    bool inverted() const;

    // Clamps every texel into [low, high]. Applied through the same absence rule as invert, so
    // clamping a field with low > 0 materialises nothing and is instead recorded as a floor.
    // A floor above 0 combined with a sparse field is exactly the "everything is at least a bit
    // masked in" case, and it must not cost 4 GiB to express.
    void clampTo(std::uint8_t low, std::uint8_t high);

    // --- Painting ---

    // Circular stroke centred on a world position, feathered from `hardness` of the radius out.
    //
    // `amount` is in [0, 1] and is scaled by the falloff, then folded with `op`. Returns the number
    // of chunks the stroke touched, which is what the caller registers for undo.
    //
    // hardness is the fraction of the radius that is fully weighted, matching Brush::getValue
    // (Brush.cpp) rather than inventing a second falloff shape for the same gesture.
    //
    // `complement` writes 1 - (amount * falloff) instead of amount * falloff, and it exists for one
    // specific and easy-to-get-wrong case: ERASING with Min.
    //
    // An erase stroke has to write 0 at the centre and leave the rim ALONE. Painting the plain
    // falloff with Min does the opposite -- the rim's falloff tends to 0, so Min drives the rim to
    // 0 and the stroke erases hardest exactly where it should touch least, turning a soft brush
    // into a hard-edged hole slightly larger than the cursor. Writing the complement makes the rim
    // tend to 255, which Min leaves untouched. The same argument applies to Multiply.
    std::size_t paintCircle( float x
                           , float z
                           , float radius
                           , float hardness
                           , float amount
                           , MaskCombine op
                           , bool complement = false
                           );

    // --- Lifetime and accounting ---

    // Drops every chunk of one tile. WIRE THIS TO TILE UNLOAD. It is what bounds a composited mask
    // to the resident tile set; without it the field grows for as long as the camera keeps moving.
    // Safe to call for a tile that holds nothing.
    void releaseTile(int tile_x, int tile_z);

    void clear();
    bool empty() const;

    std::size_t chunkCount() const;
    std::size_t denseChunkCount() const;
    std::size_t uniformChunkCount() const;

    // Bytes of texel payload plus a measured per-entry overhead. Not an estimate: see
    // MASK_ENTRY_OVERHEAD_BYTES in TerrainMask.cpp for what is counted and why.
    std::size_t bytes() const;

    // Every stored chunk, ascending by packed(). Deterministic, which is what makes a saved mask
    // byte-identical across two runs that produced the same field.
    std::vector<MaskChunkAddress> storedChunks() const;

  private:
    // Two-state block: uniform costs no payload, dense costs MASK_CHUNK_TEXELS.
    struct Block
    {
      std::uint8_t uniform = 0;
      // Null while the block is uniform. MASK_CHUNK_TEXELS bytes otherwise.
      std::unique_ptr<std::uint8_t[]> texels;
    };

    // Materialises `block` into a dense payload filled with its uniform value, if it is not dense
    // already. Returns the payload.
    static std::uint8_t* densify(Block& block);
    // Collapses a dense payload back to uniform when every texel agrees. Called after every write
    // that could have made a block uniform, because the alternative is a mask that only ever grows.
    static void collapse(Block& block);

    Block const* find(MaskChunkAddress const& address) const;

    // Applied to every value leaving the field, and to the implicit zero of an absent chunk. See
    // invert() and clampTo().
    std::uint8_t present(std::uint8_t stored) const;

    std::unordered_map<std::uint32_t, Block> _blocks;

    bool _inverted = false;
    std::uint8_t _clamp_low = 0;
    std::uint8_t _clamp_high = MASK_MAX;
  };
}

#endif // NOGGIT_TERRAINMASK_HPP
