// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_WORLDRENDER_HPP
#define NOGGIT_WORLDRENDER_HPP

#include <noggit/rendering/BaseRender.hpp>

#include <external/glm/glm.hpp>

#include <noggit/tool_enums.hpp>
#include <noggit/rendering/CursorRender.hpp>
#include <noggit/rendering/LiquidTextureManager.hpp>
#include <noggit/map_horizon.h>
#include <noggit/Sky.h>

#include <noggit/rendering/Primitives.hpp>
#include <noggit/rendering/ShadowBaker.hpp>

#include <memory>

namespace OpenGL
{
  struct program;
}

struct TileIndex;
class World;
struct MinimapRenderSettings;

// Forward-declared, so no database header reaches a rendering header. The renderer only ever
// looks spawns up through this pointer -- it issues no query and touches no DBC.
namespace Noggit::Database
{
  class SpawnSceneCache;
}

// Forward-declared for weight, not for layering. World holds a WorldRender by value
// (World.h:493), so every translation unit that reaches World.h reaches this header, and pulling
// DetailDoodadRender.hpp in here would drag ModelManager.h, Model.h, ModelRender.hpp and
// Listfile.hpp behind it into all of them. The one cost is that WorldRender now needs an
// out-of-line destructor, since a unique_ptr to an incomplete type cannot be destroyed inline.
namespace Noggit::Rendering
{
  class DetailDoodadRender;
}


struct WorldRenderParams 
{
  float cursorRotation;
  CursorType cursor_type;
  float brush_radius;
  bool show_unpaintable_chunks;
  bool draw_only_inside_light_sphere;
  bool draw_wireframe_light_sphere;
  float alpha_light_sphere;
  float inner_radius_ratio;
  float angle;
  float orientation;
  bool use_ref_pos;
  bool angled_mode;
  bool draw_paintability_overlay;
  editing_mode editing_mode;
  bool camera_moved;
  bool draw_mfbo;
  bool draw_terrain;
  bool draw_wmo;
  bool draw_water;
  bool draw_wmo_doodads;
  bool draw_models;
  bool draw_model_animations;
  bool draw_models_with_box;
  bool draw_hidden_models;
  bool draw_sky;
  bool draw_skybox;
  bool draw_fog;
  eTerrainType ground_editing_brush;
  int water_layer;
  display_mode display_mode;
  bool draw_occlusion_boxes;
  bool minimap_render;
  bool draw_wmo_exterior;

  bool render_select_m2_aabb;
  bool render_select_m2_collission_bbox;
  bool render_select_wmo_aabb;
  bool render_select_wmo_groups_bounds;

  // TrinityCore world-database spawns drawn as an overlay.
  //
  // Both default-initialised, unlike the members above, so that a null cache and a disabled
  // overlay are the behaviour of a WorldRenderParams nobody has filled in. The overlay is drawn
  // only when the toggle is on AND the pointer is non-null, so a build or a session with no
  // database configured needs no #ifdef anywhere in the render path.
  bool draw_db_spawns = false;
  Noggit::Database::SpawnSceneCache* db_spawns = nullptr;
};

namespace Noggit::Rendering
{
  class WorldRender : public BaseRender
  {
  public:
    WorldRender(World* world);

    // Out-of-line and defined as defaulted in the .cpp: _detail_doodad_render is a unique_ptr to a
    // type this header only forward-declares. See the note on that forward declaration.
    ~WorldRender();

    void upload() override;
    void unload() override;

    void draw (glm::mat4x4 const& model_view
        , glm::mat4x4 const& projection
        , glm::vec3 const& cursor_pos
        , glm::vec4 const& cursor_color
        , glm::vec3 const& ref_pos
        , glm::vec3 const& camera_pos
        , MinimapRenderSettings* minimap_render_settings
        , WorldRenderParams const& render_settings
    );

    bool saveMinimap (TileIndex const& tile_idx
                      , MinimapRenderSettings* settings
                      , std::optional<QImage>& combined_image);

    // Renders the loaded scene once from the sun's viewpoint and reads the depth buffer back, for
    // the terrain shadow (MCSH) bake. Returns false and leaves `out` invalid on failure.
    //
    // This is the GPU half; Noggit::Rendering::ShadowBaker holds the arithmetic and
    // World::bakeTerrainShadows does the chunk writes. Requires a current GL context and MUST NOT
    // be called from paintGL: it ends in a glReadPixels of up to 8192x8192 floats, which is a full
    // pipeline stall by nature. The tool calls it from a button handler, which the Qt event loop
    // delivers nowhere near a paint.
    //
    // > [!note] This is deliberately NOT the two-program approach Noggit Green uses. Green renders
    // > depth into a 2048 texture with dedicated depth shaders, then draws the terrain again at
    // > 1024 with a shadow-compare fragment shader and reads back the result. Only the first of
    // > those passes is here: the compare is done per MCSH texel on the CPU
    // > (Noggit::Rendering::bakeChunkShadowMap) against the terrain's own vertex lattice. Our
    // > renderer has no program-substitution seam for M2 and WMO drawing, so a depth-only pass
    // > would have meant four new shader programs and a second way to walk the scene; reusing
    // > draw() gets correct depth for terrain, alpha-tested doodads and WMOs for free, and moving
    // > the compare to the CPU removes the raster-alignment question entirely.
    bool renderSunDepth ( TileIndex const& tile_idx
                        , Noggit::Rendering::ShadowBakeSettings const& settings
                        , Noggit::Rendering::SunDepthMap& out
                        , int* out_neighbour_tiles_loaded = nullptr
                        );

    [[nodiscard]]
    OpenGL::TerrainParamsUniformBlock* getTerrainParamsUniformBlock();;

    void updateTerrainParamsUniformBlock();
    void markTerrainParamsUniformBlockDirty();;

    [[nodiscard]] std::unique_ptr<Skies>& skies();;

    // The in-viewport preview of a chunk's ground effect doodads. Never null.
    //
    // Reached this way rather than through WorldRenderParams because the control that drives it is
    // the Ground Effects tool itself, which already talks to the renderer directly for the
    // overlay uniforms (GroundEffectsTool::updateTerrainUniformParams). A second copy of the same
    // three settings in the per-frame parameter block would just be something else to keep in
    // sync.
    //
    // > [!warning] detailDoodads().clear() requires a bound OpenGL context. See the class comment.
    [[nodiscard]] DetailDoodadRender& detailDoodads();;

    float _view_distance;
    float cullDistance() const;

    // Per-frame budget for chunk alphamap uploads, spent at whole-tile granularity: the budget is
    // tested before a tile draws, so a tile that starts within budget always finishes its burst.
    // A tile is 256 chunks, so the old value of 256 let a second full tile through before the
    // counter exceeded it -- up to 512 chunk uploads in one paintGL while terrain streams in.
    // Any value below 256 caps that at one tile's worth. 32 leaves headroom for several
    // lightly-updated tiles (an edited tile normally dirties a handful of chunks) while still
    // capping the streaming burst. Deferral is lossless: TileRender::draw skips the whole update
    // block without clearing the chunk or tile update flags, so the tile re-enters next frame.
    unsigned int _frame_max_chunk_updates = 32;

    bool directional_lightning;
    bool local_lightning;

  private:

    void drawMinimap ( MapTile *tile
        , glm::mat4x4 const& model_view
        , glm::mat4x4 const& projection
        , glm::vec3 const& camera_pos
        , MinimapRenderSettings* settings
    );

    void updateMVPUniformBlock(const glm::mat4x4& model_view, const glm::mat4x4& projection);
    void updateLightingUniformBlock(bool draw_fog, glm::vec3 const& camera_pos);
    void updateLightingUniformBlockMinimap(MinimapRenderSettings* settings);

    void setupChunkVAO(OpenGL::Scoped::use_program& mcnk_shader);
    void setupLiquidChunkVAO(OpenGL::Scoped::use_program& water_shader);
    void setupOccluderBuffers();
    void setupChunkBuffers();
    void setupLiquidChunkBuffers();

    World* _world;
    float _cull_distance;

    // shaders
    std::unique_ptr<OpenGL::program> _mcnk_program;;
    std::unique_ptr<OpenGL::program> _mfbo_program;
    std::unique_ptr<OpenGL::program> _m2_program;
    std::unique_ptr<OpenGL::program> _m2_instanced_program;
    std::unique_ptr<OpenGL::program> _m2_particles_program;
    std::unique_ptr<OpenGL::program> _m2_ribbons_program;
    std::unique_ptr<OpenGL::program> _m2_box_program;
    std::unique_ptr<OpenGL::program> _wmo_program;
    std::unique_ptr<OpenGL::program> _liquid_program;
    std::unique_ptr<OpenGL::program> _occluder_program;

    // horizon && skies && lighting
    std::unique_ptr<Noggit::map_horizon::render> _horizon_render;
    std::unique_ptr<OutdoorLighting> _outdoor_lighting;
    OutdoorLightStats _outdoor_light_stats;
    std::unique_ptr<Skies> _skies;

    // cursor
    Noggit::CursorRender _cursor_render;
    Noggit::Rendering::Primitives::Sphere _sphere_render;
    Noggit::Rendering::Primitives::Square _square_render;
    Noggit::Rendering::Primitives::Line _line_render;
    Noggit::Rendering::Primitives::WireBox _wirebox_render;

    // buffers
    OpenGL::Scoped::deferred_upload_buffers<8> _buffers;
    GLuint const& _mvp_ubo = _buffers[0];
    GLuint const& _lighting_ubo = _buffers[1];
    GLuint const& _terrain_params_ubo = _buffers[2];
    GLuint const& _mapchunk_vertex = _buffers[3];
    GLuint const& _mapchunk_index = _buffers[4];
    GLuint const& _mapchunk_texcoord = _buffers[5];
    GLuint const& _liquid_chunk_vertex = _buffers[6];
    GLuint const& _occluder_index = _buffers[7];

    // uniform blocks
    OpenGL::MVPUniformBlock _mvp_ubo_data;
    OpenGL::LightingUniformBlock _lighting_ubo_data;
    OpenGL::TerrainParamsUniformBlock _terrain_params_ubo_data;

    // VAOs
    OpenGL::Scoped::deferred_upload_vertex_arrays<3> _vertex_arrays;
    GLuint const& _mapchunk_vao = _vertex_arrays[0];
    GLuint const& _liquid_chunk_vao = _vertex_arrays[1];
    GLuint const& _occluder_vao = _vertex_arrays[2];

    LiquidTextureManager _liquid_texture_manager;

    // Constructed once, in the constructor, and never reset -- unload() clears its contents
    // instead. Keeping the object alive across an unload/upload cycle is what lets the user's
    // enable/density/draw-distance choices survive ViewportManager handing the GL context to
    // another viewport and back.
    std::unique_ptr<DetailDoodadRender> _detail_doodad_render;

    bool _need_terrain_params_ubo_update = false;
  };
}

#endif //NOGGIT_WORLDRENDER_HPP
