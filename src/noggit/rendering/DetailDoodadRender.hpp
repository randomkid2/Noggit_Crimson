// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DETAILDOODADRENDER_HPP
#define NOGGIT_DETAILDOODADRENDER_HPP

#include <noggit/ContextObject.hpp>
#include <noggit/ModelManager.h>
#include <noggit/rendering/DetailDoodadPlacement.hpp>
#include <noggit/tool_enums.hpp>

#include <external/glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

class MapChunk;
class MapTile;
class Model;

namespace math
{
  class frustum;
}

namespace OpenGL
{
  struct M2RenderState;

  namespace Scoped
  {
    struct use_program;
  }
}

namespace Noggit::Rendering
{
  // Draws a chunk's ground effect doodads -- the grass, flowers and scrub its GroundEffectTexture
  // rows describe -- in the viewport, so that the Ground Effect Sets editor can be used without
  // launching the game to see the result.
  //
  // PROVENANCE. The placement this consumes follows Natsirt867's `ground_effects_improvements`
  // work (upstream merge request !49, closed unmerged); see DetailDoodadPlacement.hpp, which is
  // where that lineage lives. THIS file does not: his renderer is wired to his renderer and his
  // rewritten Ground Effects tool, and neither is ours. What is here is written against Noggit
  // Crimson's WorldRender and reuses its existing instanced M2 path rather than adding one.
  //
  // It owns no shader, no buffer and no VAO. A batch of detail doodads is a batch of M2 instances,
  // and WorldRender already binds a program and a render state for exactly that; this class hands
  // its transforms to the same ModelRender::draw the ADT doodad pass and the database spawn
  // overlay use. That is also why draw() takes a bound shader rather than binding one.
  //
  // > [!warning] An OpenGL context must be current when clear() runs or when this is destroyed
  // > It owns scoped_model_references. Releasing the last reference to a Model runs ~Model ->
  // > ~ModelRender, which destroys OpenGL vertex arrays, and OpenGL::Scoped's destructor throws
  // > when no context is bound -- a throw from a destructor terminates the process. The same
  // > hazard SpawnSceneCache documents.
  // >
  // > Two release points both satisfy that, and either is enough: WorldRender::unload(), which
  // > MapView::unloadOpenglData calls inside makeCurrent(), and ~GroundEffectsTool, which
  // > ~MapView reaches through _tools[editing_mode::paint].reset() while its own
  // > OpenGL::context::scoped_setter is still in scope.
  class DetailDoodadRender
  {
  public:
    explicit DetailDoodadRender(Noggit::NoggitRenderContext context);

    // Off by default. Nothing is cached, no model is referenced and no chunk is visited until a
    // user asks for the preview -- a mapper who never opens the Ground Effects tool pays nothing.
    [[nodiscard]] bool enabled() const;
    void setEnabled(bool enabled);

    // Yards. Detail doodads are numerous by design, so this is the control that decides the cost.
    [[nodiscard]] float drawDistance() const;
    void setDrawDistance(float distance);

    // The client's groundEffectDensity: how many of a chunk's 64 cells get picked. Clamped to
    // [16, 256]. Changing it invalidates every cached chunk, because it changes the random stream
    // rather than merely scaling the result.
    [[nodiscard]] unsigned density() const;
    void setDensity(unsigned density);

    // Drops every cached placement and releases every model reference. Requires a bound context;
    // see the warning above.
    void clear();

    // --- per-frame, called from WorldRender::draw in this order ---

    // Resets the batches and the per-frame rebuild budget. Cheap: the batch vectors keep their
    // capacity between frames, so a steady view allocates nothing.
    void beginFrame();

    // Adds every chunk of one tile that is inside the draw distance and the frustum. Safe to call
    // for a tile that is nowhere near: the first thing it does is one distance test against the
    // whole tile.
    void gatherTile(MapTile* tile, glm::vec3 const& camera_pos, math::frustum const& frustum);

    // One instanced draw per distinct model, into an already-bound M2 program.
    void draw( glm::mat4x4 const& model_view
             , OpenGL::Scoped::use_program& m2_shader
             , OpenGL::M2RenderState& model_render_state
             , math::frustum const& frustum
             , float cull_distance
             , glm::vec3 const& camera_pos
             , int animtime
             , std::unordered_map<Model*, std::size_t>& model_boxes_to_draw
             , display_mode display
             );

    // What the last frame actually cost, for the read-out in the Ground Effects tool. Reporting
    // measured numbers rather than an estimate is the only way a user can tell whether the draw
    // distance they picked is affordable on their machine.
    [[nodiscard]] std::size_t lastFrameInstanceCount() const;
    [[nodiscard]] std::size_t lastFrameChunkCount() const;
    [[nodiscard]] std::size_t lastFrameRebuildCount() const;
    [[nodiscard]] std::size_t cachedChunkCount() const;

  private:
    // One chunk's placements, and enough to tell whether they are still the right ones.
    struct ChunkCache
    {
      // Everything the placement reads other than the heightmap, hashed. 44 bytes: the MCLY
      // doodad mapping (8 x uint16), the exclusion stencil (8 x uint8), the four layers' effect
      // ids (4 x uint32) and the hole mask. Hashing beats watching ChunkUpdateFlags because the
      // flags are noisier than the data -- TileRender re-registers ALPHAMAP every frame while a
      // tile streams -- and because a repaint that does not move the doodad mapping genuinely does
      // not move a doodad, which is the common case under Live Auto Texture.
      std::uint64_t signature = 0;

      // The heightmap, watched by counter rather than by hash: 145 vertices per chunk is too much
      // to re-hash per chunk per frame, and MapChunk::registerChunkUpdate already knows exactly
      // when they moved.
      std::uint32_t surface_revision = 0;

      // Rebuilt when the density changes, since density is a seed-stream input.
      unsigned density = 0;

      std::vector<DetailDoodadGroup> groups;

      std::uint64_t last_used_frame = 0;
    };

    // A GroundEffectDoodad row resolved to a held model. Empty `reference` means the row named
    // nothing usable; the entry is still kept so the failure is not re-resolved every frame.
    struct DoodadModel
    {
      std::optional<scoped_model_reference> reference;
    };

    // Identity of a chunk that survives the chunk being destroyed and a new one landing on the
    // same address. Without it a recycled MapChunk* could inherit the previous chunk's placements
    // whenever the signature happened to match; with it, a match means the same chunk in the same
    // place holding the same data, in which case reusing the cache is right.
    [[nodiscard]] static std::uint32_t chunkIdentity(MapChunk* chunk);

    [[nodiscard]] static std::uint64_t chunkSignature(MapChunk* chunk);

    // Resolves a GroundEffectDoodad row to a model, holding the reference so the asynchronous
    // load survives to completion. Returns nullptr while it is still loading or if it failed.
    [[nodiscard]] Model* resolveModel(std::uint32_t doodad_id);

    Noggit::NoggitRenderContext _context;

    bool _enabled = false;

    // 120 yards. Chosen against a measured number rather than by feel: a chunk is 33.33 yards, so
    // 120 covers pi * 120^2 / 33.33^2 = 40.7 chunks, and at the default density that is roughly
    // ten thousand instances -- a batch size this codebase's instanced M2 path already carries for
    // ADT doodads on a populated tile.
    float _draw_distance = 120.0f;

    // 32, not the client's 16. The client renders these against a full doodad set; here the point
    // is to see what a set looks like, and 16 picks over 64 cells leaves a chunk visibly patchy.
    unsigned _density = 32;

    std::unordered_map<MapChunk*, ChunkCache> _chunk_cache;
    std::unordered_map<std::uint32_t, DoodadModel> _models;

    // Held across frames so the transform vectors keep their capacity. clear() on a vector does
    // not release its buffer, so a steady view does no allocation at all after the first frame.
    std::unordered_map<Model*, std::vector<glm::mat4x4>> _batches;

    // Ceiling on how many chunks may be rebuilt in one frame.
    //
    // A rebuild is the only expensive thing here -- roughly density * (2 + amount * 5) generator
    // steps -- and Live Auto Texture retextures every chunk a stroke touched in one go, so without
    // a budget the frame that ends a large stroke would rebuild all of them at once. Deferral is
    // lossless: a chunk that does not get rebuilt keeps its old placements for one more frame and
    // is picked up on the next, because nothing clears the mismatch that made it a candidate.
    //
    // 8 matches the shape of WorldRender::_frame_max_chunk_updates, which caps alphamap uploads
    // the same way and for the same reason.
    static constexpr std::size_t MAX_REBUILDS_PER_FRAME = 8;

    // Cached placements for a chunk nobody has looked at in this many frames are dropped. At 60fps
    // that is ten seconds -- long enough that turning around and back costs nothing, short enough
    // that flying across a map does not accumulate the whole map's grass in memory.
    static constexpr std::uint64_t CACHE_LIFETIME_FRAMES = 600;

    // How often the sweep runs. Walking the cache every frame to find stale entries would cost
    // more than the entries do; 256 frames is roughly four seconds.
    static constexpr std::uint64_t SWEEP_INTERVAL_FRAMES = 256;

    std::uint64_t _frame = 0;
    std::size_t _rebuilds_this_frame = 0;
    std::size_t _instances_last_frame = 0;
    std::size_t _chunks_last_frame = 0;
    std::size_t _rebuilds_last_frame = 0;
  };
}

#endif // NOGGIT_DETAILDOODADRENDER_HPP
