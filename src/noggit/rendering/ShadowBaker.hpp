// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_RENDERING_SHADOWBAKER_HPP
#define NOGGIT_RENDERING_SHADOWBAKER_HPP

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

// Baking terrain shadows (MCSH) from a sun-viewpoint depth render.
//
// ---------------------------------------------------------------------------------------------
// What MCSH is, and how it differs from the ambient-occlusion bake next door
//
// These are two different layers with two different meanings and they must never be merged.
//
//   MCCV, written by Noggit::AmbientOcclusionBaker (terrain/AmbientOcclusion.hpp)
//     Per-VERTEX colour, 145 values per chunk, a multiplier the shader applies to the lit
//     result (terrain_frag.glsl:353, out_color.rgb *= vary_mccv). It is DIRECTIONLESS: the AO
//     estimator integrates the whole hemisphere, so it darkens crevices and gullies no matter
//     where the sun is. It is a property of the shape of the ground.
//
//   MCSH, written by this module
//     Per-TEXEL, 64x64 one-bit values per chunk, composited by the client as the SUN shadow.
//     In this tree that composite is terrain_frag.glsl:464-468 -- a single .r sample off the
//     shadowmap array, used as out_color.rgb * (1 - shadow_alpha). It is DIRECTIONAL: it is the
//     answer to "does anything stand between this piece of ground and the sun", so it is where
//     a building's or a cliff's cast shadow lives, and it is the layer Blizzard's own ADTs ship
//     populated and custom terrain ships empty.
//
// The two compose multiplicatively and are meant to be used together. Baking AO does not give
// you cast shadows and baking shadows does not give you contact darkening. Anybody tempted to
// unify them should note that they do not even share a resolution: MCCV is 145 vertices per
// chunk and MCSH is 4096 texels.
//
// ---------------------------------------------------------------------------------------------
// The storage was already here. Measured before any of this was written:
//
//   read    MapChunk.cpp:227-270  MCSH is 0x200 bytes, unpacked LSB-first into _shadow_map as
//                                 85 for a set bit and 0 for a clear one
//   write   MapChunk.cpp:1880-1907  gated on has_shadows(); an all-zero map writes no MCSH block
//                                 at all and clears mcnk flag has_mcsh
//   pack    MapChunk.cpp:371-384  compressed_shadow_map(), bit i of byte i/8 for texel i
//   upload  MapChunk.cpp:1454  texSubImage3D into layer px*16+py of a GL_R8 array
//   undo    Action.cpp:898-910 / :221-228  registerChunkShadowChange + ActionFlags::eCHUNK_SHADOWS
//   copy    ChunkClipboard.cpp:305-309, :830-833  the Chunk Manipulator's SHADOWS data class
//
// So MCSH round-trips through load, edit, undo, copy and save already, and this module adds NO
// serialisation. What was missing upstream is only the two producers: a brush and a bake.
// Noggit Red has exactly one shadow method, World::clear_shadows, which erases.
//
// ---------------------------------------------------------------------------------------------
// The resolution arithmetic, checked rather than quoted
//
// The claim worth verifying is that a 1024x1024 readback fitted to one tile lands on MCSH texels
// one-to-one with no resampling. In IEEE single precision, with the constants this tree actually
// uses (MapHeaders.h:58-62):
//
//   TILESIZE       533.3333129882812
//   CHUNKSIZE      = TILESIZE / 16   =  33.33333206176758
//   TEXDETAILSIZE  = CHUNKSIZE / 64  =   0.5208333134651184
//   TILESIZE / 1024                  =   0.5208333134651184     <- bit-identical
//
// It is not merely close, it is exact, and it has to be: /16, /64 and /1024 are all divisions by
// powers of two, which are exact in binary floating point, so (x/16)/64 and x/1024 cannot
// disagree. 16 chunks x 64 texels = 1024 across a tile, and 64*64/8 = 512 = 0x200 bytes, which is
// the MCSH block size the writer already hardcodes at MapChunk.cpp:1886.
//
// This module therefore does NOT rasterise a 1024x1024 image and slice it. It goes further and
// addresses each MCSH texel directly by its world position, which makes the one-to-one mapping a
// property of the code rather than a property of an alignment that has to be got right. The
// 1024 figure survives as the thing the depth resolution should comfortably exceed.
//
// ---------------------------------------------------------------------------------------------
// The split through this header
//
// Everything declared here is pure and depends on nothing but glm and the STL -- no OpenGL, no
// MapChunk, no Qt. The GPU half (rendering the scene from the sun into a depth buffer) is
// WorldRender::renderSunDepth, and the world walk (undo, chunk writes) is
// World::bakeTerrainShadows. Keeping the arithmetic here means the fiddly parts -- the light
// basis, the orthographic fit, the depth convention, the barycentric height -- can be reasoned
// about and unit-tested without a GL context.
namespace Noggit::Rendering
{
  // The byte written for a shadowed texel.
  //
  // 85 and NOT 255, which is the single easiest thing to get wrong here. The loader unpacks a set
  // MCSH bit to 85 (MapChunk.cpp:250), the texture is GL_R8 sampled as a normalised float, and
  // the shader multiplies by (1 - that) -- so 85/255 = 1/3 is a third off the ground's brightness
  // and is what every Blizzard-authored shadow in the game looks like in this editor. Painting
  // 255 would produce black shadows three times the intended strength that nonetheless save and
  // reload as ordinary ones, because the save path only asks whether a texel is non-zero
  // (MapChunk.cpp:379). The discrepancy would be visible only until the next reload, which is the
  // worst way for a bug to behave.
  inline constexpr std::uint8_t MCSH_SHADOW_VALUE = 85;

  // MCSH is 64x64 per chunk (MapChunk.h:106), 8 texels per terrain cell across the chunk's 8x8.
  inline constexpr int MCSH_RESOLUTION = 64;
  inline constexpr int MCSH_TEXEL_COUNT = MCSH_RESOLUTION * MCSH_RESOLUTION;

  // MCVT/MCCV/MCNR vertex count per chunk: 9*9 outer + 8*8 inner (MapChunk.h:46, mapbufsize).
  inline constexpr int CHUNK_VERTEX_COUNT = 145;

  // Restated from MapHeaders.h:58-60 rather than included, so this header stays free of a header
  // that opens with <glm/vec3.hpp> and drags the map format in behind it. Values checked against
  // the originals; see the arithmetic note above.
  inline constexpr float TILE_SIZE_YARDS = 533.33333f;
  inline constexpr float CHUNK_SIZE_YARDS = TILE_SIZE_YARDS / 16.0f;
  inline constexpr float UNIT_SIZE_YARDS = CHUNK_SIZE_YARDS / 8.0f;
  inline constexpr float MCSH_TEXEL_SIZE_YARDS = CHUNK_SIZE_YARDS / 64.0f;

  // MapChunk.h:52, ChunkUpdateFlags::SHADOW. Restated for the same reason.
  inline constexpr unsigned CHUNK_UPDATE_FLAG_SHADOW = 0x2;

  struct ShadowBakeSettings
  {
    // Elevation of the sun above the horizon, degrees. Noggit Green's control is 1..90 with a
    // default of 47 and this follows it: 90 is noon straight overhead and casts almost nothing,
    // 1 is a sunset that throws shadows the length of the tile.
    //
    // The lower bound is 1 rather than 0 on purpose. At 0 the light is exactly horizontal, the
    // orthographic box fitted to the tile degenerates to zero thickness in the vertical axis and
    // the projection is singular.
    float sun_pitch_degrees = 47.0f;

    // Azimuth of the sun, degrees, measured in the XZ plane: 0 puts the sun toward -X so light
    // travels toward +X.
    //
    // Green fixes the yaw; this exposes it but keeps a fixed DEFAULT, which is the property that
    // actually matters -- two bakes of neighbouring tiles with untouched settings agree, so
    // shadows do not change direction at a tile boundary. Changing it between tiles is a way to
    // produce a map that looks wrong in a way that is very hard to diagnose later, and the panel
    // says so.
    float sun_yaw_degrees = 0.0f;

    // Coverage above which a texel is written as shadowed, on Green's 0..256 scale with its
    // default of 128. Each MCSH texel is sampled `supersample` squared times across its own
    // footprint and the fraction in shadow is scaled to 0..255; a texel is set when that reaches
    // this value. 0 shadows everything the box covers, 256 shadows nothing.
    int threshold = 128;

    // Edge of the square depth render, in texels. Green uses 2048 for the depth pass.
    //
    // Worth understanding before lowering it. The depth map covers the tile as seen FROM THE SUN,
    // so its footprint is the tile's 533 yards divided by sin(pitch) in one axis: 2048 texels is
    // 0.36 yards each at the default 47 degrees, comfortably finer than the 0.52-yard MCSH texel
    // it feeds, but only 1.0 yards each at a 15-degree sun, which is coarser than MCSH and shows
    // as stair-stepped shadow edges. Low sun wants a higher resolution, and the panel reports the
    // ratio it actually achieved rather than leaving the user to work it out.
    int depth_resolution = 2048;

    // How far the sample point is nudged toward the sun before its depth is compared, in yards.
    //
    // This is shadow-map bias expressed in the one unit a map author can reason about. Too little
    // and gently lit slopes self-shadow into moire (acne); too much and small objects lose the
    // contact shadow at their base. Because the projection is orthographic the world-to-depth
    // scale is a single constant, so this converts exactly -- see shadowBiasInDepthUnits.
    float bias_yards = 1.0f;

    // Samples per axis inside each MCSH texel, so the real count is this squared. 1 is a single
    // point at the texel centre and aliases badly on shadow edges; 2 is four samples and is the
    // default; 4 is sixteen and is the finishing-pass setting. This is what makes `threshold`
    // meaningful at all -- with one sample per texel the coverage can only ever be 0 or 255 and
    // every threshold between 1 and 255 behaves identically.
    int supersample = 2;

    // How far outside the tile, in yards, geometry is still allowed to cast into it.
    //
    // Needed in two directions at once and used for both: sideways, because a tower on the
    // neighbouring tile throws a shadow across the border, and toward the sun, because the
    // orthographic box has to start far enough back to contain the caster rather than slicing
    // through it. 200 yards at the default 47-degree sun catches anything up to ~215 yards tall.
    //
    // It cannot conjure geometry that is not loaded. A caster on an unloaded tile is not in the
    // scene and casts nothing however large this is, which is why the bake reports how many
    // neighbouring tiles were resident when it ran.
    float caster_margin_yards = 200.0f;

    // Whether M2 doodads and WMOs are drawn into the depth pass. Off, the bake is terrain-only:
    // cliffs still shade the valleys below them, buildings and trees cast nothing. Useful mainly
    // for comparing against a previous terrain-only bake.
    bool include_models = true;
    bool include_wmos = true;

    // Clamps every field into a range with defined behaviour. Idempotent.
    [[nodiscard]] ShadowBakeSettings sanitized() const;
  };

  // The result of rendering the scene once from the sun.
  //
  // `depth` is glReadPixels output: window-space depth in [0, 1], `resolution` wide, stored
  // BOTTOM ROW FIRST because that is GL's framebuffer origin and flipping a 16 MB buffer to match
  // a different convention would be pure cost. sampleDepth below indexes it accordingly.
  struct SunDepthMap
  {
    int resolution = 0;
    std::vector<float> depth;

    // The sun's view and orthographic projection, and their product.
    //
    // Kept separately as well as combined because the renderer needs them apart. Handing draw()
    // the product as its projection and an identity model-view would give the right gl_Position
    // -- the shaders only ever use them multiplied for that -- but m2_vert.glsl:96-107 also uses
    // model_view ALONE, for the view-space position that feeds environment-map UV generation and
    // the fog distance. An identity model-view there puts world coordinates where view
    // coordinates belong, which changes which texels an alpha-tested doodad discards and so
    // changes the shape of the shadow a tree casts.
    glm::mat4 light_view = glm::mat4(1.0f);
    glm::mat4 light_projection = glm::mat4(1.0f);

    // Maps world space to clip space; w is 1 because the projection is orthographic, which is
    // what lets the depth comparison be a subtraction.
    glm::mat4 light_view_projection = glm::mat4(1.0f);

    // The orthographic near and far planes, in yards. Kept because the world-to-depth conversion
    // needs them and reading them back out of the matrix is a needless opportunity for error.
    float near_plane = 0.0f;
    float far_plane = 0.0f;

    [[nodiscard]] bool valid() const;
  };

  // What one tile's bake actually did.
  //
  // Every field exists so the tool can report rather than assert. A bake is a slow, once-per-tile
  // finishing operation whose output is invisible until the terrain is next lit, so "it ran" is
  // not a useful thing to tell the user: the failure that matters is a bake that completes and
  // writes nothing, and the only defence against that is reporting the numbers.
  struct ShadowBakeReport
  {
    // Null on success, otherwise a short reason the bake did not run at all.
    char const* failure = nullptr;

    // Chunks the walk visited, and of those, the ones whose 64x64 map actually differs from what
    // it was. A re-bake with identical settings correctly reports 256 visited and 0 changed.
    int chunks_visited = 0;
    int chunks_changed = 0;

    // Texels set across the whole tile, out of 256 * 4096 = 1048576. Zero after a successful run
    // means the sun genuinely reaches every part of this tile, which at a high pitch over flat
    // ground is the right answer and at a low pitch over cliffs means something is wrong.
    int texels_shadowed = 0;

    // Loaded tiles other than this one that were in the scene when the depth pass ran. A caster
    // on an unloaded tile is not in the scene and casts nothing, so a border shadow that seems to
    // be missing is usually this number being 0.
    int neighbour_tiles_loaded = 0;

    // Yards per texel of the depth render, and of MCSH. The first should be comfortably smaller
    // than the second; when it is not, shadow edges come out stair-stepped and the fix is a
    // higher depth resolution or a higher sun.
    float depth_texel_yards = 0.0f;
    float mcsh_texel_yards = MCSH_TEXEL_SIZE_YARDS;

    [[nodiscard]] bool ok() const { return failure == nullptr; }

    // True when the depth pass resolved the tile more finely than MCSH does, which is the
    // condition under which the threshold control behaves the way the panel describes.
    [[nodiscard]] bool depthOutresolvesMcsh() const
    {
      return depth_texel_yards > 0.0f && depth_texel_yards <= mcsh_texel_yards;
    }
  };

  // Unit direction the sunlight TRAVELS, from the sun toward the ground. Always normalised and
  // always finite; a pitch of 90 gives exactly (0, -1, 0).
  [[nodiscard]] glm::vec3 sunLightDirection(float pitch_degrees, float yaw_degrees);

  // Builds the sun's view and orthographic projection so that the box [min_bounds, max_bounds],
  // grown by `caster_margin_yards` on every side and by the same amount toward the sun, is
  // entirely inside the frustum.
  //
  // This is the part worth following Green on rather than reinventing, and the shape is theirs:
  // aim a look-at down the light direction, transform the eight corners of the world box into
  // that view space, and take the componentwise extremes as the orthographic bounds. It is the
  // standard tight fit and it is correct for any pitch and yaw, which a hand-derived special case
  // for "sun overhead" is not.
  //
  // Two traps are handled here and neither is obvious from the formula:
  //
  //   1. At a pitch of 90 the light direction is exactly (0, -1, 0) and the conventional up
  //      vector (0, 1, 0) is antiparallel to it. glm::lookAt cross-products those into a zero
  //      vector and normalises it, so every element of the matrix becomes NaN and every texel
  //      silently comes back unshadowed. The up vector is swapped for (0, 0, 1) whenever the
  //      light is within about 2.5 degrees of vertical.
  //
  //   2. The eye is pushed back along the light until the whole box is in front of it, so the
  //      near plane is positive and equals caster_margin_yards exactly. Leaving the eye at the
  //      box centre would work arithmetically -- an orthographic near plane may be negative --
  //      but it makes the depth values mean something different on either side of the centre and
  //      is a poor thing to hand to anyone debugging a readback.
  //
  // Returns false, leaving `out` untouched, for a degenerate box or a non-finite setting.
  [[nodiscard]] bool makeSunTransform( glm::vec3 const& min_bounds
                                     , glm::vec3 const& max_bounds
                                     , ShadowBakeSettings const& settings
                                     , SunDepthMap& out
                                     );

  // The world-space bias converted into the [0, 1] window-depth units glReadPixels returns.
  //
  // Exact rather than approximate, and that is a property of the projection being orthographic.
  // glm::ortho with the default [-1, 1] depth range (this tree defines neither
  // GLM_FORCE_DEPTH_ZERO_TO_ONE nor GLM_FORCE_LEFT_HANDED) gives
  // ndc_z = (2d - far - near) / (far - near) for a point d yards in front of the eye, so window
  // depth is (d - near) / (far - near) -- linear in d, with a constant scale of 1/(far - near).
  // A perspective projection would have no such constant and this whole approach would need a
  // slope-scaled bias instead.
  [[nodiscard]] float shadowBiasInDepthUnits(SunDepthMap const& map, float bias_yards);

  // Nearest-texel read of the depth map at normalised coordinates. Returns a negative value for a
  // coordinate outside [0, 1], which is how "the sun's view did not cover this point" is
  // signalled; callers must treat that as lit rather than as depth zero.
  [[nodiscard]] float sampleDepth(SunDepthMap const& map, float u, float v);

  // True when `world_pos` has something between it and the sun.
  //
  // `bias_depth_units` comes from shadowBiasInDepthUnits. A point the sun's view does not cover
  // is reported LIT, never shadowed: inventing shadow where there is no data would put a hard
  // black edge exactly at the boundary of whatever was loaded, which is both wrong and the most
  // alarming possible way to be wrong.
  [[nodiscard]] bool isPointInShadow( SunDepthMap const& map
                                    , glm::vec3 const& world_pos
                                    , float bias_depth_units
                                    );

  // Terrain height at a point inside one chunk, from that chunk's 145 MCVT vertices.
  //
  // `local_x` and `local_z` are offsets from the chunk origin in yards, in [0, CHUNK_SIZE_YARDS];
  // outside that they are clamped to the chunk rather than extrapolated.
  //
  // Interpolates over the SAME four-triangle fan the renderer draws, not a bilinear patch over
  // the four corners, because WoW terrain cells are not planar quads: each 8x8 cell has a fifth
  // vertex at its centre carrying its own height (MapChunk.cpp:178-193, the odd rows), and a
  // bilinear read would miss every ridge and gully that lives on those centre vertices. The
  // vertex indexing is the loader's own: outer vertex (row, column) is at row*17 + column and the
  // cell centre for (row, column) is at row*17 + 9 + column, which is exactly what
  // MapChunk::indexNoLoD and MapChunk::indexLoD compute (MapChunk.cpp:354-362).
  [[nodiscard]] float terrainHeightInChunk( glm::vec3 const* vertices
                                          , float local_x
                                          , float local_z
                                          );

  // World XZ of the CENTRE of MCSH texel (column, row) of a chunk whose origin is (xbase, zbase).
  //
  // Column is the x axis and row is the z axis, which is not a coin flip: the shadow array is
  // uploaded with texSubImage3D at width 64 (MapChunk.cpp:1454), so index row*64 + column, and
  // the shader samples it at vary_texcoord/8 (terrain_frag.glsl:466) where texcoord is built as
  // (x_local/UNITSIZE, z_local/UNITSIZE) (WorldRender.cpp:2259-2260 against the vertex positions
  // at MapChunk.cpp:183-186). The loader's do-not-fix-alpha-map correction agrees: it repairs
  // "column 63 of every row" as _shadow_map[i*64 + 63] (MapChunk.cpp:259).
  [[nodiscard]] glm::vec3 mcshTexelCentre(float xbase, float zbase, int column, int row);

  // Fills `out_shadow_map`, which must have room for 64*64 bytes, for one chunk.
  //
  // `vertices` is the chunk's 145-entry mVertices array and `xbase`/`zbase` its world origin.
  // Every texel is sampled settings.supersample squared times across its own footprint; the
  // fraction found in shadow is scaled to 0..255 and compared with settings.threshold. Written
  // values are MCSH_SHADOW_VALUE or 0 and nothing else.
  //
  // Returns the number of texels set, so a caller can tell "this chunk is genuinely in full sun"
  // from "the bake did nothing" -- which is the distinction a bake tool most needs to be able to
  // report and the one it is easiest to leave the user guessing about.
  int bakeChunkShadowMap( SunDepthMap const& map
                        , glm::vec3 const* vertices
                        , float xbase
                        , float zbase
                        , ShadowBakeSettings const& settings
                        , std::uint8_t* out_shadow_map
                        );
}

#endif // NOGGIT_RENDERING_SHADOWBAKER_HPP
