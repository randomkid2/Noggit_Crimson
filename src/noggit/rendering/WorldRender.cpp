// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include "WorldRender.hpp"
#include "WMOGroupRender.hpp"
#include <external/PNG2BLP/Png2Blp.h>
#include <external/tracy/Tracy.hpp>
#include <math/frustum.hpp>
#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/database/SpawnSceneCache.hpp>
#include <noggit/DBC.h>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/TileIndex.hpp>
#include <noggit/MinimapRenderSettings.hpp>
#include <noggit/Misc.h>
#include <noggit/rendering/DetailDoodadRender.hpp>
#include <noggit/reports/MissingPlacementLog.hpp>
#include <noggit/Model.h>
#include <noggit/ModelInstance.h>
#include <noggit/rendering/PlaceholderCube.hpp>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/World.h>

#include <noggit/ui/MinimapCreator.hpp>

#include <opengl/shader.hpp>

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QListWidget>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFramebufferObjectFormat>
#include <QSettings>

#include <algorithm>

using namespace Noggit::Rendering;

WorldRender::WorldRender(World* world)
: BaseRender()
, _world(world)
, _liquid_texture_manager(world->_context)
, _view_distance(world->_settings->value("view_distance", 2000.f).toFloat() + TILE_RADIUS) // add adt radius to make sure tiles aren't culled too soon, todo: improve adt culling to prevent that from happening
, _cull_distance(0.f)
, directional_lightning(world->_settings->value("directional_lightning", true).toBool())
, local_lightning(world->_settings->value("local_lightning", true).toBool())
{
  // Constructed here rather than in upload() so that the settings the Ground Effects tool writes
  // into it survive an unload/upload cycle. It holds no OpenGL object of its own, so building it
  // outside a bound context is safe; only clearing it is not.
  _detail_doodad_render = std::make_unique<DetailDoodadRender>(world->_context);
}

// Defaulted, but it has to live here: DetailDoodadRender is only forward-declared in the header,
// and destroying a unique_ptr needs the complete type.
Noggit::Rendering::WorldRender::~WorldRender() = default;

void WorldRender::draw (glm::mat4x4 const& model_view
    , glm::mat4x4 const& projection
    , glm::vec3 const& cursor_pos
    , glm::vec4 const& cursor_color
    , glm::vec3 const& ref_pos
    , glm::vec3 const& camera_pos
    , MinimapRenderSettings* minimap_render_settings
    , WorldRenderParams const& render_settings
)
{

  ZoneScoped;

  glm::mat4x4 const mvp(projection * model_view);
  math::frustum const frustum (mvp);

  if (render_settings.camera_moved)
    updateMVPUniformBlock(model_view, projection);

  gl.disable(GL_DEPTH_TEST);

  if (!render_settings.minimap_render)
  {
    int daytime = static_cast<int>(_world->time) % 2880;
    // always render local lights in sky/lightning editing mode.
    bool render_local_lightning = render_settings.editing_mode == editing_mode::light ? true : local_lightning;
    _skies->update_sky_colors(camera_pos, daytime, !render_local_lightning);
    updateLightingUniformBlock(render_settings.draw_fog, camera_pos);
  }
  else
  {
    updateLightingUniformBlockMinimap(minimap_render_settings);
  }

  // setup render settings for minimap
  if (render_settings.minimap_render)
  {
    _terrain_params_ubo_data.draw_shadows = minimap_render_settings->draw_shadows;
    _terrain_params_ubo_data.draw_lines = minimap_render_settings->draw_adt_grid;
    _terrain_params_ubo_data.draw_terrain_height_contour = minimap_render_settings->draw_elevation;
    _terrain_params_ubo_data.draw_hole_lines = false;
    _terrain_params_ubo_data.draw_impass_overlay = false;
    _terrain_params_ubo_data.draw_areaid_overlay = false;
    _terrain_params_ubo_data.draw_paintability_overlay = false;
    _terrain_params_ubo_data.draw_selection_overlay = false;
    _terrain_params_ubo_data.draw_wireframe = false;
    _terrain_params_ubo_data.draw_groundeffectid_overlay = false;
    _terrain_params_ubo_data.draw_groundeffect_layerid_overlay = false;
    _terrain_params_ubo_data.draw_noeffectdoodad_overlay = false;
    _terrain_params_ubo_data.draw_only_normals = minimap_render_settings->draw_only_normals;
    _terrain_params_ubo_data.point_normals_up = minimap_render_settings->point_normals_up;
    _need_terrain_params_ubo_update = true;
  }

  // After coming out of minimap rendering mode and draw_only_normals is still on, disable it.
  if (!render_settings.minimap_render && _terrain_params_ubo_data.draw_only_normals) {
      _terrain_params_ubo_data.draw_only_normals = false;
      _need_terrain_params_ubo_update = true;
  }

  // After coming out of minimap rendering mode and point_normals_up is still on, disable it.
  if (!render_settings.minimap_render && _terrain_params_ubo_data.point_normals_up) {
      _terrain_params_ubo_data.point_normals_up = false;
      _need_terrain_params_ubo_update = true;
  }

  if (_need_terrain_params_ubo_update)
    updateTerrainParamsUniformBlock();

  // Frustum culling
  _world->_n_loaded_tiles = 0;
  unsigned tile_counter = 0;

  bool modern_features = Noggit::Application::NoggitApplication::instance()->getConfiguration()->modern_features;

  for (MapTile* tile : _world->mapIndex.loaded_tiles())
  {
    tile->_was_rendered_last_frame = false;

    if (render_settings.minimap_render)
    {
      auto& tile_extents = tile->getCombinedExtents();
      tile->calcCamDist(camera_pos);
      tile->renderer()->setFrustumCulled(false);
      tile->renderer()->setObjectsFrustumCullTest(2);
      tile->renderer()->setOccluded(false);
      _world->_loaded_tiles_buffer[tile_counter] = std::make_pair(std::make_pair(static_cast<int>(tile->index.x), static_cast<int>(tile->index.z)), tile);

      tile_counter++;
      _world->_n_loaded_tiles++;
      continue;
    }

    auto& tile_extents = tile->getCombinedExtents();
    if (frustum.intersects(tile_extents[1], tile_extents[0]) || tile->getChunkUpdateFlags())
    {
      tile->calcCamDist(camera_pos);
      _world->_loaded_tiles_buffer[tile_counter] = std::make_pair(std::make_pair(static_cast<int>(tile->index.x), static_cast<int>(tile->index.z)), tile);

      tile->renderer()->setObjectsFrustumCullTest(1);
      if (frustum.contains(tile_extents[0]) && frustum.contains(tile_extents[1]))
      {
        tile->renderer()->setObjectsFrustumCullTest( tile->renderer()->objectsFrustumCullTest() + 1);
      }

      if (tile->renderer()->isFrustumCulled())
      {
        tile->renderer()->setOverrideOcclusionCulling(true);
        tile->renderer()->discardTileOcclusionQuery();
        tile->renderer()->setOccluded(false);
      }

      tile->renderer()->setFrustumCulled(false);

      tile_counter++;
    }
    else
    {
      tile->renderer()->setFrustumCulled(true);
      tile->renderer()->setObjectsFrustumCullTest(0);
    }

    _world->_n_loaded_tiles++;
  }

  auto buf_end = _world->_loaded_tiles_buffer.begin() + tile_counter;

  // The buffer holds exactly 64*64 entries, so a full map with every tile loaded and passing the
  // frustum test leaves tile_counter == size() and this null terminator one past the end. The
  // consumers below all iterate the whole array and break on the null pointer, so when the buffer
  // is completely full there is nothing left to terminate -- iteration ends on its own.
  if (tile_counter < _world->_loaded_tiles_buffer.size())
  {
    _world->_loaded_tiles_buffer[tile_counter] = std::make_pair<std::pair<int, int>, MapTile*>(std::make_pair<int, int>(0, 0), nullptr);
  }


  // It is always import to sort tiles __front to back__.
  // Otherwise selection would not work. Overdraw overhead is gonna occur as well.
  // TODO: perhaps parallel sort?
  std::sort(_world->_loaded_tiles_buffer.begin(), buf_end,
            [](std::pair<std::pair<int, int>, MapTile*>& a, std::pair<std::pair<int, int>, MapTile*>& b) -> bool
            {
              if (!a.second)
              {
                return false;
              }

              if (!b.second)
              {
                return true;
              }

              return a.second->camDist() < b.second->camDist();
            });

  // only draw the sky in 3D
  if(!render_settings.minimap_render && render_settings.display_mode == display_mode::in_3D && render_settings.draw_sky)
  {
    ZoneScopedN("World::draw() : Draw skies");
    OpenGL::Scoped::use_program m2_shader {*_m2_program.get()};

    bool hadSky = false;

    if (render_settings.draw_skybox && (render_settings.draw_wmo || _world->mapIndex.hasAGlobalWMO()))
    {
      _world->_model_instance_storage.for_each_wmo_instance
          (
              [&] (WMOInstance& wmo)
              {
                if (wmo.wmo->finishedLoading() && wmo.wmo->skybox)
                {
                  if (wmo.getGroupExtents().empty())
                  {
                    wmo.recalcExtents();
                  }

                  hadSky = wmo.wmo->renderer()->drawSkybox(model_view
                      , camera_pos
                      , m2_shader
                      , frustum
                      , _cull_distance
                      , _world->animtime
                      , render_settings.draw_model_animations
                      , wmo.getExtents()[0]
                      , wmo.getExtents()[1]
                      , wmo.getGroupExtents()
                  );
                }

              }
              , [&] () { return hadSky; }
          );
    }

    if (!hadSky)
    {
      _skies->draw( model_view
          , projection
          , camera_pos
          , m2_shader
          , frustum
          , _cull_distance
          , _world->animtime
          , _world->time
          , render_settings.draw_skybox
          , _outdoor_light_stats
      );
    }
  }

  _cull_distance= render_settings.draw_fog ? _skies->fog_distance_end() : _view_distance;

  // Draw verylowres heightmap
  if (!_world->mapIndex.hasAGlobalWMO() && render_settings.draw_fog && render_settings.draw_terrain)
  {
    ZoneScopedN("World::draw() : Draw horizon");
    _horizon_render->draw (model_view, projection, 
      &_world->mapIndex, _skies->color_set[SKY_FOG_COLOR],
      _cull_distance,
      frustum,
      camera_pos,
      render_settings.display_mode);
  }

  gl.enable(GL_DEPTH_TEST);
  gl.depthFunc(GL_LEQUAL); // less z-fighting artifacts this way, I think
  //gl.disable(GL_BLEND);
  gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  //gl.disable(GL_CULL_FACE);

  _world->_n_rendered_tiles = 0;
  _world->_n_rendered_objects = 0;

  if (render_settings.draw_terrain)
  {
    ZoneScopedN("World::draw() : Draw terrain");

    gl.disable(GL_BLEND);

    {
      OpenGL::Scoped::use_program mcnk_shader{ *_mcnk_program.get() };

      mcnk_shader.uniform("enable_mists_heightmapping", modern_features);
      mcnk_shader.uniform("camera", glm::vec3(camera_pos.x, camera_pos.y, camera_pos.z));
      mcnk_shader.uniform("animtime", static_cast<int>(_world->animtime));

      if (render_settings.cursor_type != CursorType::NONE)
      {
        mcnk_shader.uniform("draw_cursor_circle", static_cast<int>(render_settings.cursor_type));
        mcnk_shader.uniform("cursor_position", glm::vec3(cursor_pos.x, cursor_pos.y, cursor_pos.z));
        mcnk_shader.uniform("cursorRotation", render_settings.cursorRotation);
        mcnk_shader.uniform("outer_cursor_radius", render_settings.brush_radius);
        mcnk_shader.uniform("inner_cursor_ratio", render_settings.inner_radius_ratio);
        mcnk_shader.uniform("cursor_color", cursor_color);
      }
      else
      {
        mcnk_shader.uniform("draw_cursor_circle", 0);
      }

      gl.bindVertexArray(_mapchunk_vao);
      gl.bindBuffer(GL_ELEMENT_ARRAY_BUFFER, _mapchunk_index);

      int num_chunks_uploaded_alphamap = 0;

      for (auto const& pair : _world->_loaded_tiles_buffer)
      {
        MapTile* tile = pair.second;

        if (!tile)
        {
          break;
        }

        if (render_settings.minimap_render)
          tile->renderer()->setOccluded(false);

        if (tile->renderer()->isOccluded() && !tile->getChunkUpdateFlags() && !tile->renderer()->isOverridingOcclusionCulling())
          continue;

        // skipping unfinished adts really improves performance so we don't have to reuplaod them every frame
        if (!tile->texturesFinishedLoading())
          continue;

        // Limit rate uploading alphamap data to avoid long frame times (causes freezes)
        // TODO make it dynamic based on target frame time and last frame times
        bool skip_updates = false;
        if (num_chunks_uploaded_alphamap > _frame_max_chunk_updates)
          skip_updates = true;

        tile->renderer()->draw(
            mcnk_shader
            , camera_pos
            , render_settings.show_unpaintable_chunks
            , render_settings.draw_paintability_overlay
            , render_settings.editing_mode == editing_mode::minimap
              && minimap_render_settings->selected_tiles.at(64 * tile->index.x + tile->index.z)
            , skip_updates
        );

        num_chunks_uploaded_alphamap += tile->renderer()->numUploadedChunkAlphamaps();

        // if (tile->renderer()->alphamapUploadedLastFrame())
        //   num_tiles_uploaded_alphamap++;

        _world->_n_rendered_tiles++;
        tile->_was_rendered_last_frame = true;

      }

      gl.bindVertexArray(0);
      gl.bindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }
  }

  if (render_settings.editing_mode == editing_mode::object && _world->has_multiple_model_selected())
  {
    ZoneScopedN("World::draw() : Draw pivot point");
    if (_world->_multi_select_pivot.has_value())
    {
      OpenGL::Scoped::bool_setter<GL_DEPTH_TEST, GL_FALSE> const disable_depth_test;

      float dist = glm::distance(camera_pos, _world->_multi_select_pivot.value());
      _sphere_render.draw(mvp, _world->_multi_select_pivot.value(), cursor_color, std::min(2.f, std::max(0.15f, dist * 0.02f)));
    }
    else
    {
      // assert(false);
    }
  }

  if (render_settings.use_ref_pos)
  {
    ZoneScopedN("World::draw() : Draw ref pos");
    _sphere_render.draw(mvp, ref_pos, cursor_color, 0.3f);
  }

  if (render_settings.editing_mode == editing_mode::ground && render_settings.ground_editing_brush == eTerrainType_Vertex)
  {
    ZoneScopedN("World::draw() : Draw vertex points");
    float size = glm::distance(_world->vertexCenter(), camera_pos);
    gl.pointSize(std::max(0.001f, 10.0f - (1.25f * size / CHUNKSIZE)));

    for (glm::vec3 const* pos : _world->_vertices_selected)
    {
      _sphere_render.draw(mvp, *pos, glm::vec4(1.f, 0.f, 0.f, 1.f), 0.5f);
    }

    _sphere_render.draw(mvp, _world->vertexCenter(), cursor_color, 2.f);
  }

  // A model_with_particles map was *declared* here. Nothing ever inserted into it: its only
  // consumers, the particle and ribbon passes further down, are commented out, and so was every
  // line that would have filled it. Removing the declaration cost one empty container's
  // construction per frame, not a gather. Reinstate it together with those passes, not before.

  tsl::robin_map<Model*, std::vector<glm::mat4x4>> models_to_draw;
  std::vector<WMOInstance*> wmos_to_draw;
  std::unordered_map<Model*, std::size_t> model_boxes_to_draw;

  // Placements whose model or WMO failed to load. Filled in the three branches below and drawn
  // once, after the model boxes, as PlaceholderCube.
  //
  // Before this existed a failed placement was drawn as NOTHING -- ModelRender::draw and
  // WMOInstance::draw both return at the top on loading_failed() -- so the mapper's map had a
  // hole in it with no indication that anything was meant to be there. It is collected here
  // rather than being handed straight to a draw call because a draw call per object inside the
  // gather loop would break the one program bind the batch is worth.
  std::vector<Noggit::Rendering::Primitives::PlaceholderCubeInstance> placeholders_to_draw;

  // Never in a generated minimap: a placeholder is a diagnostic about the editor's client data,
  // and baking one into a .blp that ships to players would be a real defect rather than a
  // cosmetic one.
  //
  // The toggle is read from MissingPlacementLog rather than from render_settings because
  // WorldRenderParams -- where it belongs, beside draw_models_with_box -- is in a header being
  // edited by other work in this change set. Read once per frame, not per object.
  bool const draw_placeholders
    (!render_settings.minimap_render && Noggit::MissingPlacementLog::instance().drawPlaceholders());

  // Database spawns, grouped by (model, display id) rather than by model alone.
  //
  // The extra key is not incidental. A creature M2 carries no skin of its own -- the mesh is
  // shared and the image comes from the display id -- so one wolf model serves wolves of several
  // colours, and grouping only by model would draw them all with whichever skin was applied last.
  // Each group is drawn with its own textures bound, which is why these cannot simply be appended
  // to models_to_draw the way they were before skins were handled.
  struct DatabaseSpawnGroup
  {
    std::vector<glm::mat4x4> transforms;

    // Borrowed from the scene cache, which owns the references. Never constructed here: a texture
    // reference created per frame and released when the swap is undone drops its refcount to zero
    // before the asynchronous BLP load finishes, so the model renders black forever.
    std::vector<std::pair<int, scoped_blp_texture_reference>> const* skins = nullptr;
  };

  std::map<std::pair<Model*, std::uint32_t>, DatabaseSpawnGroup> db_spawns_to_draw;

  // frame counter loop. pretty hacky but works
  // this is used to make sure no object is processed more than once within a frame
  static int frame = 0;

  if (frame == std::numeric_limits<int>::max())
  {
    frame = 0;
  }
  else
  {
    frame++;
  }

  // Detail doodads are gathered inside the tile loop below, so the batches have to be reset
  // before it starts. Costs nothing when the preview is off: gatherTile returns on its first line.
  _detail_doodad_render->beginFrame();

  for (auto const& pair : _world->_loaded_tiles_buffer)
  {
    MapTile* tile = pair.second;

    if (!tile)
    {
      break;
    }

    if (render_settings.minimap_render)
      tile->renderer()->setOccluded(false);

    if (tile->renderer()->isOccluded() && !tile->getChunkUpdateFlags() && !tile->renderer()->isOverridingOcclusionCulling())
      continue;

    // early dist check
    // TODO: optional
    if (tile->camDist() > _cull_distance)
      continue;


    // TODO: subject to potential generalization
    for (auto& pair : tile->getObjectInstances())
    {
      if (!pair.first->finishedLoading())
        continue;

      // MISSING ASSET PLACEHOLDERS.
      //
      // Handled here, above the eMODEL/eWMO split, because the two branches below immediately
      // reinterpret_cast the key to a Model* or the instances to WMOInstance* and then read
      // members that a failed load never wrote. Catching the failure first means neither branch
      // has to be made defensive.
      //
      // Deliberately NOT gated on render_settings.draw_models or draw_wmo. Hiding models is how
      // a mapper looks at terrain; hiding the markers that say "a model is broken here" at the
      // same time would remove them from the one view where they are easiest to find. The
      // dedicated toggle in the View menu is what turns these off.
      if (pair.first->loading_failed())
      {
        for (auto& instance : pair.second)
        {
          instance->_rendered_last_frame = false;
        }

        if (!draw_placeholders || pair.second.empty())
        {
          continue;
        }

        Noggit::MissingPlacementKind const placeholder_kind
          ( pair.second[0]->which() == eWMO ? Noggit::MissingPlacementKind::WorldModel
                                            : Noggit::MissingPlacementKind::Model
          );

        for (auto& instance : pair.second)
        {
          if (instance->frame == frame)
          {
            continue;
          }

          instance->frame = frame;

          // getExtents() is virtual and both subclasses now return the placeholder cube for a
          // failed asset, so this is the same box the ray test in intersect() uses -- there is
          // exactly one definition of where a placeholder is.
          auto const& placeholder_extents = instance->getExtents();

          if (!frustum.intersects(placeholder_extents[1], placeholder_extents[0]))
          {
            continue;
          }

          if (glm::distance(camera_pos, instance->pos) > _cull_distance)
          {
            continue;
          }

          Noggit::Rendering::Primitives::PlaceholderCubeInstance placeholder;
          placeholder.centre = instance->pos;
          placeholder.kind = placeholder_kind;
          placeholder.selected = _world->is_selected(instance->uid);

          placeholders_to_draw.push_back(placeholder);
        }

        continue;
      }

      if (pair.second[0]->which() == eMODEL)
      {
        if (!render_settings.draw_models && !(render_settings.minimap_render && minimap_render_settings->use_filters))
        {
          // can optimize this with a tile.rendered_m2s_lastframe or just check if models are enabled
          for (auto& instance : pair.second)
          {
            instance->_rendered_last_frame = false;
          }
          continue;
        }

        auto& instances = models_to_draw[reinterpret_cast<Model*>(pair.first)];

        // memory allocation heuristic. all objects will pass if tile is entirely in frustum.
        // otherwise we only allocate for a half

        if (tile->renderer()->objectsFrustumCullTest() > 1)
        {
          instances.reserve(instances.size() + pair.second.size());
        }
        else
        {
          instances.reserve(instances.size() + pair.second.size() / 2);
        }


        for (auto& instance : pair.second)
        {
          instance->_rendered_last_frame = false;

          // do not render twice the cross-referenced objects twice
          if (instance->frame == frame)
          {
            instance->_rendered_last_frame = true;
            continue;
          }

          auto m2_instance = static_cast<ModelInstance*>(instance);

          if (!render_settings.draw_hidden_models && m2_instance->model->is_hidden())
            continue;

          instance->frame = frame;

          bool render = false;
          // experimental : if camera and object haven't moved/changed since last frame, we don't need to do frustum culling again
          if (!render_settings.camera_moved && !m2_instance->extentsDirty()/* && not_moved*/)
          {
            if (m2_instance->_rendered_last_frame)
            {
              render = true; // skip frustum check
            }
          }
          if (!render && m2_instance->isInRenderDist(_cull_distance, camera_pos, render_settings.display_mode)
            && (tile->renderer()->objectsFrustumCullTest() > 1 || m2_instance->isInFrustum(frustum)))
          {
            render = true;
          }

          if (!render)
            continue;

          instances.emplace_back(m2_instance->transformMatrix());
          m2_instance->_rendered_last_frame = true;


          // if (render && !draw_models_with_box /* && !m2_instance->model->is_hidden()*/)
          // {
          //   // model box wasn't set in model draw(), add selection boxes
          //   if (_world->selected_uids.contains(m2_instance->uid))
          //     model_boxes_to_draw.emplace(m2_instance->model, instances.size());
          // }

        }

      }
      else if (pair.second[0]->which() == eWMO)
      {
        if (!render_settings.draw_wmo)
        {
          for (auto& instance : pair.second)
          {
            instance->_rendered_last_frame = false;
          }
          continue;
        }

        // memory allocation heuristic. all objects will pass if tile is entirely in frustum.
        // otherwise we only allocate for a half

        if (tile->renderer()->objectsFrustumCullTest() > 1)
        {
          wmos_to_draw.reserve(wmos_to_draw.size() + pair.second.size());
        }
        else
        {
          wmos_to_draw.reserve(wmos_to_draw.size() + pair.second.size() / 2);
        }

        for (auto& instance : pair.second)
        {
          instance->_rendered_last_frame = false;

          // do not render twice the cross-referenced objects twice
          if (instance->frame == frame)
          {
            instance->_rendered_last_frame = true;
            continue;
          }

          auto wmo_instance = static_cast<WMOInstance*>(instance);

          if (!render_settings.draw_hidden_models && wmo_instance->wmo->is_hidden())
            continue;

          instance->frame = frame;

          // experimental : if camera and object haven't moved/changed since last frame, we don't need to do frustum culling again
          bool render = false;
          if (!render_settings.camera_moved && !wmo_instance->extentsDirty()/* && not_moved*/)
          {
            if (wmo_instance->_rendered_last_frame)
            {
              render = true; // skip visibility checks
            }
          }
          // The parentheses are load-bearing for cost, not for outcome. Without them this parses
          // as ((!render && contained) || intersects), which still evaluates the frustum test
          // even when the coherence shortcut above has already decided to render. Same result,
          // one frustum test per already-decided WMO instance per frame wasted. Matches the M2
          // path above, which is written this way already.
          if (!render && (tile->renderer()->objectsFrustumCullTest() > 1
                          || frustum.intersects(wmo_instance->getExtents()[1], wmo_instance->getExtents()[0])))
          {
            render = true;
          }

          if (render)
          {
            wmos_to_draw.emplace_back(wmo_instance);
            wmo_instance->_rendered_last_frame = true;

            if (render_settings.draw_wmo_doodads)
            {
              // auto doodads = wmo_instance->get_visible_doodads(frustum, _cull_distance, camera_pos, draw_hidden_models, display);
              // 
              // for (auto& doodad : doodads)
              // {
              //     if (doodad->frame == frame)
              //         continue;
              //     doodad->frame = frame;
              // 
              //     auto& instances = models_to_draw[doodad->model.get()];
              // 
              //     instances.emplace_back(doodad->transformMatrix());
              // }

              // doodad->isInFrustum(frustum);

              std::map<uint32_t, std::vector<wmo_doodad_instance>>* doodads = wmo_instance->get_doodads(render_settings.draw_hidden_models);
              
              if (!doodads)
                continue;
              
              for (auto& pair : *doodads)
              {
                for (auto& doodad : pair.second)
                {
                    if (doodad.frame == frame)
                        continue;
                    doodad.frame = frame;

                    // Nothing in this loop checked finishedLoading(), and the two lines further
                    // down read doodad.model->bounding_box_radius, which is written only by a
                    // successful load. A doodad still streaming in was therefore measured
                    // against an indeterminate float every frame until it arrived. Skipping it
                    // costs nothing: ModelRender::draw returns at the top for the same condition,
                    // so it was never drawn on those frames either.
                    if (!doodad.model->finishedLoading())
                      continue;

                    // A doodad named in the WMO's MODD chunk whose .m2 is not in the client data.
                    // This is the case nothing in the program distinguished before: the building
                    // is there and one of its props silently is not, so the mapper sees a room
                    // that looks finished and is not, with no way to find out which model is
                    // wanted short of opening the .wmo in an external tool.
                    //
                    // It also has to be caught HERE, before the two lines below: `dist` reads
                    // doodad.model->bounding_box_radius and isInRenderDist reads it again, and
                    // that float is indeterminate for a model that failed to load. That garbage
                    // read predates this feature -- nothing in this loop filtered failed models.
                    //
                    // world_pos, not pos: a WMO doodad's pos is in the owning WMO's local space.
                    // update_transform_matrix_wmo gates only on finishedLoading(), which is true
                    // after a failure, so world_pos and the extents it feeds are both valid.
                    if (doodad.model->loading_failed())
                    {
                      if (!draw_placeholders)
                        continue;

                      auto const& doodad_extents = doodad.getExtents();

                      if (!frustum.intersects(doodad_extents[1], doodad_extents[0]))
                        continue;

                      if (glm::distance(camera_pos, doodad.world_pos) > _cull_distance)
                        continue;

                      Noggit::Rendering::Primitives::PlaceholderCubeInstance placeholder;
                      placeholder.centre = doodad.world_pos;
                      placeholder.kind = Noggit::MissingPlacementKind::WorldModelDoodad;

                      // Never highlighted as selected, because a WMO doodad is not a selectable
                      // placement -- it has no MDDF entry and no uid of its own. The handle for
                      // fixing it is the owning WMO, whose uid the Missing Objects panel shows.
                      placeholder.selected = false;

                      placeholders_to_draw.push_back(placeholder);

                      continue;
                    }

                    // skip no geometry boxes for WMO doodads
                    if (doodad.model->use_fake_geometry())
                      continue;

                    // apply size culling to wmo doodads?
                    float dist = glm::distance(camera_pos, doodad.world_pos) - (doodad.model->bounding_box_radius * doodad.scale);

                    if (!doodad.isInRenderDist(_cull_distance, camera_pos, render_settings.display_mode))
                      continue;
                    // TODO can check if in indoor group & exterior not hidden for further optimization. possibly check portals relations
              
                    auto& instances = models_to_draw[doodad.model.get()];
              
                    instances.emplace_back(doodad.transformMatrix());
                }
              }
            }
          }
        }
      }
    }

    // TrinityCore world-database spawns.
    //
    // Appended into the same models_to_draw the MDDF path above fills, so the overlay costs one
    // instanced draw call per distinct model and needs no shader, no render state and no second
    // pass of its own -- the consumer further down handles it unchanged.
    //
    // Inside the tile loop deliberately: these then inherit the tile-level occlusion and distance
    // culling applied at the top of it, exactly as ADT objects do. And note what is NOT touched --
    // MapTile::object_instances. The ADT save path walks that index, so a database spawn cannot
    // reach MDDF/MODF by any route, which is the hard rule made structural instead of remembered.
    //
    // Skipped entirely for a minimap render: these are server-side data and have no business in a
    // generated minimap, and the per-model include filter below would in any case reject them.
    if (render_settings.draw_db_spawns && render_settings.db_spawns && !render_settings.minimap_render)
    {
      if (auto const* spawn_scene = render_settings.db_spawns->tile(tile->index))
      {
        for (auto const& entry : spawn_scene->entries)
        {
          ModelInstance* spawn_instance = entry.instance.get();

          // The same guard the MDDF path applies to its map key. Models are streamed in
          // asynchronously, so on the first frames after a load most of these are not ready yet;
          // a failed load stays false forever and is skipped rather than drawn as nothing.
          if (!spawn_instance->model->finishedLoading() || spawn_instance->model->loading_failed())
            continue;

          // Extents are resolved here rather than as a side effect of isInRenderDist, because
          // that function is deliberately not used below. recalcExtents defers when the model is
          // still loading, so without this the frustum test runs against the opposite-infinity
          // initial values.
          spawn_instance->ensureExtents();

          // Culling by the view distance directly, NOT via ModelInstance::isInRenderDist.
          //
          // That function applies a size-based ladder -- `size_cat < 1 && dist > 300` and so on
          // (ModelInstance.cpp:243) -- and size_cat is only ever populated from the MDDF/WDT path,
          // so it is 0 for every database spawn. Every creature would therefore vanish beyond 300
          // yards, which is barely half an ADT tile: the spawns would be culled while the tile
          // holding them was still plainly on screen. Correct for scenery doodads, useless for a
          // placement overlay whose whole job is showing you where things are.
          float const distance
            ( glm::distance(camera_pos, spawn_instance->pos)
            - spawn_instance->model->bounding_box_radius * spawn_instance->scale
            );

          if (distance >= _cull_distance)
            continue;

          if (tile->renderer()->objectsFrustumCullTest() <= 1 && !spawn_instance->isInFrustum(frustum))
            continue;

          // Outline the selected spawn.
          //
          // Without this, picking a row in the spawn panel gives no feedback at all, and a tile
          // holding twenty of the same creature is unworkable -- every one of them looks like the
          // one you picked. Drawn per instance here rather than in the instanced pass below,
          // which deliberately knows nothing about individual spawns.
          //
          // Amber, to stay distinguishable from the white current-selection and yellow collision
          // boxes the ADT object path already draws (ModelInstance.cpp:93,102).
          if (entry.ref().valid() && entry.ref() == render_settings.db_spawns->selected())
          {
            auto const& extents = spawn_instance->getExtents();

            Noggit::Rendering::Primitives::WireBox::getInstance(_world->_context).draw
              ( model_view
              , projection
              , glm::mat4x4(1.0f)
              , {1.0f, 0.85f, 0.15f, 1.0f}
              , extents[0]
              , extents[1]
              );
          }

          auto& group
            (db_spawns_to_draw[{spawn_instance->model.get(), entry.display_id}]);

          group.transforms.emplace_back(spawn_instance->transformMatrix());

          // Every entry sharing this key has the same display id and therefore the same skins,
          // so the first one to arrive settles it.
          if (!group.skins)
          {
            group.skins = &entry.skin_textures;
          }
        }
      }
    }

    // Ground effect detail doodads, gathered inside the tile loop for the same reason the database
    // spawns are: it inherits the occlusion and distance culling applied at the top of it. Its own
    // draw distance is much shorter than the terrain view distance, so on a normal frame this
    // returns after one distance test for all but a handful of tiles.
    //
    // Deliberately NOT gated on render_settings.draw_models. Hiding doodads is how a mapper looks
    // at terrain, and the ground effect preview is a terrain overlay -- it answers "what does the
    // set I just painted look like", which is a question about the ground, not about the M2s
    // placed on it. Its own toggle in the Ground Effects tool is what turns it off.
    _detail_doodad_render->gatherTile(tile, camera_pos, frustum);
  }

  // WMOs / map objects
  if (render_settings.draw_wmo || _world->mapIndex.hasAGlobalWMO())
  {
    ZoneScopedN("World::draw() : Draw WMOs");
    {
      OpenGL::Scoped::use_program wmo_program{*_wmo_program.get()};

      wmo_program.uniform("camera", glm::vec3(camera_pos.x, camera_pos.y, camera_pos.z));

      // make this check per WMO or global WMO with tiles may not work
      bool disable_cull = false;

      if (_world->mapIndex.hasAGlobalWMO() && !wmos_to_draw.size())
      {
          auto global_wmo = _world->_model_instance_storage.get_wmo_instance(_world->mWmoEntry.uniqueID);
          if (global_wmo.has_value())
          {
            wmos_to_draw.push_back(global_wmo.value());
            disable_cull = true;
          }
      }


      for (auto& instance: wmos_to_draw)
      {
        bool is_hidden = instance->wmo->is_hidden();

        bool is_exclusion_filtered = false;

        // minimap render exclusion filters
        // per-model
        if (render_settings.minimap_render && minimap_render_settings->use_filters)
        {
          if (instance->instance_model()->file_key().hasFilepath())
          {
            for(int i = 0; i < minimap_render_settings->wmo_model_filter_exclude->count(); ++i)
            {
              auto item = reinterpret_cast<Ui::MinimapWMOModelFilterEntry*>(
                  minimap_render_settings->wmo_model_filter_exclude->itemWidget(
                  minimap_render_settings->wmo_model_filter_exclude->item(i)));

              if (item->getFileName().toStdString() == instance->instance_model()->file_key().filepath())
              {
                is_exclusion_filtered = true;
                break;
              }
            }
          }

          // per-instance
          for(int i = 0; i < minimap_render_settings->wmo_instance_filter_exclude->count(); ++i)
          {
            auto item = reinterpret_cast<Ui::MinimapInstanceFilterEntry*>(
                minimap_render_settings->wmo_instance_filter_exclude->itemWidget(
                minimap_render_settings->wmo_instance_filter_exclude->item(i)));

            if (item->getUid() == instance->uid)
            {
              is_exclusion_filtered = true;
              break;
            }
          }

          // skip model rendering if excluded by filter
          if (is_exclusion_filtered)
            continue;
        }

        bool const is_selected = _world->selected_uids.contains(instance->uid);

        /*if (draw_hidden_models || !is_hidden)*/ // now checking when adding instances
        {
          instance->draw(wmo_program
              , model_view
              , projection
              , frustum
              , _cull_distance
              , camera_pos
              , is_hidden
              , render_settings.draw_wmo_doodads
              , render_settings.draw_fog
              , is_selected
              , _world->animtime
              , _skies->hasSkies()
              , render_settings.display_mode
              , disable_cull
              , render_settings.draw_wmo_exterior
              , render_settings.render_select_wmo_aabb
              , render_settings.render_select_wmo_groups_bounds

          );
        }
      }
    }
  }


  // occlusion culling
  // terrain tiles act as occluders for each other, water and M2/WMOs.
  // occlusion culling is not performed on per model instance basis
  // rendering a little extra is cheaper than querying.
  // occlusion latency has 1-2 frames delay.

  constexpr bool occlusion_cull = true;
  if (occlusion_cull)
  {
    OpenGL::Scoped::use_program occluder_shader{ *_occluder_program.get() };
    gl.colorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    gl.depthMask(GL_FALSE);
    gl.bindVertexArray(_occluder_vao);
    gl.bindBuffer(GL_ELEMENT_ARRAY_BUFFER, _occluder_index);
    gl.disable(GL_CULL_FACE); // TODO: figure out why indices are bad and we need this

    for (auto const& pair : _world->_loaded_tiles_buffer)
    {
      MapTile* tile = pair.second;

      if (!tile)
      {
        break;
      }

      tile->renderer()->setOccluded(!tile->renderer()->getTileOcclusionQueryResult(camera_pos));
      tile->renderer()->doTileOcclusionQuery(occluder_shader);
    }

    gl.enable(GL_CULL_FACE);
    gl.colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.depthMask(GL_TRUE);
    gl.bindVertexArray(0);
    gl.bindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
  }


  // draw occlusion AABBs
  if (render_settings.draw_occlusion_boxes)
  {

    for (auto const& pair : _world->_loaded_tiles_buffer)
    {
      MapTile* tile = pair.second;

      if (!tile)
      {
        break;
      }

      glm::mat4x4 identity_mtx = glm::mat4x4{1};
      auto& extents = tile->getCombinedExtents();
      Noggit::Rendering::Primitives::WireBox::getInstance(_world->_context).draw ( model_view
          , projection
          , identity_mtx
          , { 1.0f, 1.0f, 0.0f, 1.0f }
          , extents[0]
          , extents[1]
      );
    }
  }

  bool draw_doodads_wmo = render_settings.draw_wmo && render_settings.draw_wmo_doodads;
  // M2s / models
  //
  // draw_db_spawns belongs on THIS gate, not only on the inner one further down.
  //
  // It was added to the inner condition alone, which is unreachable unless this one already
  // passed -- so the added term was a strict no-op and the bug it claimed to fix was still live:
  // pressing F1 (Doodads) and F2 (WMO doodads), or Shift+F1 which clears all three at once, made
  // this false and discarded every gathered database spawn while the View menu still showed
  // "Database spawns" ticked. The amber selection outline kept drawing, because it is emitted
  // from the gather loop rather than from this pass, leaving a box floating around nothing.
  if (render_settings.draw_models || draw_doodads_wmo || render_settings.draw_db_spawns
    || (render_settings.minimap_render && minimap_render_settings->use_filters))
  {
    ZoneScopedN("World::draw() : Draw M2s");

    if (render_settings.draw_model_animations)
    {
      ModelManager::resetAnim();
    }
    /*
    if (_world->need_model_updates)
    {
      _world->update_models_by_filename();
    }*/

    {
      // draw_db_spawns has to be part of this condition, not just of the gathering above. The
      // database overlay shares models_to_draw with the MDDF path, so without it turning "Doodads"
      // off (F1) would silently drop every database spawn as well while its own toggle still
      // showed as on -- the map is populated, nothing consumes it.
      if (render_settings.draw_models || draw_doodads_wmo || render_settings.draw_db_spawns
        || (render_settings.minimap_render && minimap_render_settings->use_filters))
      {
        OpenGL::Scoped::use_program m2_shader {*_m2_instanced_program.get()};

        OpenGL::M2RenderState model_render_state;
        model_render_state.tex_arrays = {0, 0};
        model_render_state.tex_indices = {0, 0};
        model_render_state.tex_unit_lookups = {0, 0};
        gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        gl.disable(GL_BLEND);
        gl.depthMask(GL_TRUE);
        gl.enable(GL_CULL_FACE);
        m2_shader.uniform("blend_mode", 0);
        m2_shader.uniform("unfogged", static_cast<int>(model_render_state.unfogged));
        m2_shader.uniform("unlit",  static_cast<int>(model_render_state.unlit));
        m2_shader.uniform("tex_unit_lookup_1", 0);
        m2_shader.uniform("tex_unit_lookup_2", 0);
        m2_shader.uniform("pixel_shader", 0);

        for (auto const& pair : models_to_draw)
        {
          bool is_inclusion_filtered = false;

          // minimap render inclusion filters
          // per-model
          if (render_settings.minimap_render && minimap_render_settings->use_filters)
          {
            if (pair.first->file_key().hasFilepath())
            {
              for(int i = 0; i < minimap_render_settings->m2_model_filter_include->count(); ++i)
              {
                auto item = reinterpret_cast<Ui::MinimapM2ModelFilterEntry*>(
                    minimap_render_settings->m2_model_filter_include->itemWidget(
                    minimap_render_settings->m2_model_filter_include->item(i)));

                if (item->getFileName().toStdString() == pair.first->file_key().filepath())
                {
                  is_inclusion_filtered = true;
                  break;
                }
              }
            }

            // skip model rendering if excluded by filter
            if (!is_inclusion_filtered)
              continue;
          }

          bool draw_animated_boxes = true;

          /*if (draw_hidden_models || !pair.first->is_hidden())*/ // now done when building models_to_draw
          {
            pair.first->renderer()->draw( model_view
                , pair.second
                , m2_shader
                , model_render_state
                , frustum
                , _cull_distance
                , camera_pos
                , _world->animtime
                , render_settings.draw_models_with_box
                , model_boxes_to_draw
                , render_settings.display_mode
                , false
                , render_settings.draw_model_animations
                , render_settings.editing_mode == editing_mode::object
                , draw_animated_boxes
            );
            _world->_n_rendered_objects += pair.second.size();
          }

          // Draw animated bounding boxes for small animated models that move
          if (/*render_settings.editing_mode == editing_mode::object*/
            (render_settings.draw_models_with_box || pair.first->is_hidden()) // same condition to draw bounding box in draw()
            /*&& render_settings.draw_model_animations*/
            && pair.first->animated_mesh()  && pair.first->mesh_bounds_ratio < 0.5f)
          {
            auto animated_bb = pair.first->getAnimatedBoundingBox();
            for (auto const& instance_matrix : pair.second)
            {
              Noggit::Rendering::Primitives::WireBox::getInstance(_world->_context).draw(model_view
                , projection
                , instance_matrix
                , { 0.6f, 0.6f, 0.6f, 1.0f } // grey
                , animated_bb[0]
                , animated_bb[1]
              );
            }
          }

        }

        // Database spawns, one instanced call per (model, display id), with that display's skins
        // bound for the duration of the call.
        //
        // Deliberately after the loop above and inside the same shader scope: it reuses the M2
        // program, the render state and the same ModelRender::draw entry point, so nothing about
        // the M2 pass is duplicated here. Only the textures differ.
        //
        // The swap is scoped to each draw and undone immediately. Model::_textures belongs to the
        // shared Model, so leaving a skin applied would repaint every other user of that model --
        // including, on a later frame, an ADT doodad that happens to reference the same file.
        // Restoring is what keeps this pass invisible to everything else.
        for (auto& spawn_group : db_spawns_to_draw)
        {
          Model* const spawn_model = spawn_group.first.first;

          if (!spawn_model || spawn_group.second.transforms.empty())
            continue;

          // Saved by copy, restored by move. scoped_blp_texture_reference has a copy constructor
          // and a move assignment but its copy assignment is deleted, which is exactly the shape
          // this needs and the reason it is written as a pair vector rather than a plain swap.
          std::vector<std::pair<std::size_t, scoped_blp_texture_reference>> saved_textures;

          if (spawn_group.second.skins)
          {
            for (auto const& skin : *spawn_group.second.skins)
            {
              for (std::size_t slot = 0; slot < spawn_model->_replaceable_texture_types.size(); ++slot)
              {
                // Matched by texture type, never by position: a model may declare its replaceable
                // slots in any order and may declare only some of them.
                if (spawn_model->_replaceable_texture_types[slot] != skin.first
                 || slot >= spawn_model->_textures.size())
                {
                  continue;
                }

                saved_textures.emplace_back(slot, spawn_model->_textures[slot]);

                // Copy-construct from the cache's held reference, then move-assign. The cache keeps
                // its own copy alive, so the texture stays loaded between frames.
                spawn_model->_textures[slot] = scoped_blp_texture_reference(skin.second);
              }
            }
          }

          bool draw_animated_boxes = true;

          spawn_model->renderer()->draw( model_view
              , spawn_group.second.transforms
              , m2_shader
              , model_render_state
              , frustum
              , _cull_distance
              , camera_pos
              , _world->animtime
              , render_settings.draw_models_with_box
              , model_boxes_to_draw
              , render_settings.display_mode
              , false
              , render_settings.draw_model_animations
              , false
              , draw_animated_boxes
          );

          for (auto& saved : saved_textures)
          {
            spawn_model->_textures[saved.first] = std::move(saved.second);
          }

          // Re-point this model's box count at the batch that was just uploaded.
          //
          // The box pass below calls ModelRender::drawBox, which issues one instanced draw of
          // `count` boxes against ModelRender::_transform_buffer -- the single per-model buffer
          // that ModelRender::draw overwrites with its own instances on every call
          // (ModelRender.cpp:235). So the recorded count is only meaningful as "the size of the
          // LAST batch uploaded for this model this frame".
          //
          // ModelRender::draw records it with emplace (ModelRender.cpp:213,222), which keeps the
          // FIRST count it is offered. Upstream that is the same thing: models_to_draw is keyed by
          // Model*, so a model is drawn exactly once per frame and first == last. This pass breaks
          // that invariant -- db_spawns_to_draw is keyed by (model, display id) and runs after the
          // models_to_draw loop, so one model can be drawn several times per frame, and the count
          // that survives belongs to a batch the transform buffer no longer holds. When it is the
          // larger of the two, the box pass fetches instance transforms past the end of the
          // buffer; when it is smaller, boxes are silently dropped.
          //
          // Only an existing entry is corrected, never created. Whether an entry is there at all
          // is draw()'s decision about whether this model wants boxes, and manufacturing one here
          // would outline models that asked for none.
          //
          // What this cannot recover: if the same model was also drawn as an ADT or WMO doodad
          // above, that batch's transforms are already gone from the buffer, so its boxes are not
          // drawn this frame. There is one transform buffer per model and the box pass runs once,
          // after everything -- so the choice is between the spawn boxes and out-of-bounds reads,
          // not between the spawn boxes and the doodad ones.
          auto const box_entry (model_boxes_to_draw.find(spawn_model));

          if (box_entry != model_boxes_to_draw.end())
          {
            box_entry->second = spawn_group.second.transforms.size();
          }
        }

        // Ground effect detail doodads: one instanced call per distinct model, reusing this
        // program, this render state and the same ModelRender::draw entry point the two passes
        // above use. Last of the three on purpose -- it draws far more instances than either, and
        // going last means the depth buffer is already populated with the models that matter when
        // the grass starts rejecting fragments.
        //
        // It records no box counts, so it cannot disturb the correction the database spawn pass
        // just made: hidden models never reach its batches at all.
        _detail_doodad_render->draw( model_view
            , m2_shader
            , model_render_state
            , frustum
            , _cull_distance
            , camera_pos
            , _world->animtime
            , model_boxes_to_draw
            , render_settings.display_mode
        );

        /*
        if (draw_doodads_wmo)
        {
          _model_instance_storage.for_each_wmo_instance([&] (WMOInstance& wmo)
            {
              auto doodads = wmo.get_doodads(draw_hidden_models);

              if (!doodads)
                return;

              static std::vector<ModelInstance*> instance_temp = {nullptr};
              for (auto& pair : *doodads)
              {
                for (auto& doodad : pair.second)
                {
                  instance_temp[0] = &doodad;
                  doodad.model->draw( model_view
                    , instance_temp
                    , m2_shader
                    , model_render_state
                    , frustum
                    , culldistance
                    , camera_pos
                    , animtime
                    , draw_models_with_box
                    , model_boxes_to_draw
                    , display
                  );
                }

              }
            });
        }

                  */
      }

    }

    gl.disable(GL_BLEND);
    gl.enable(GL_CULL_FACE);
    gl.depthMask(GL_TRUE);


    // unsigned int wmos_todraw_count = wmos_to_draw.size();
    // unsigned int models_todraw_count = models_to_draw.size();
    _world->_n_rendered_objects += wmos_to_draw.size();

    models_to_draw.clear();
    wmos_to_draw.clear();

    // draw model boxes with m2 box shader
    // if(draw_models_with_box || (draw_hidden_models && !model_boxes_to_draw.empty()))
    if (!render_settings.minimap_render && !model_boxes_to_draw.empty())
    {
      OpenGL::Scoped::use_program m2_box_shader{ *_m2_box_program.get() };

      OpenGL::Scoped::bool_setter<GL_LINE_SMOOTH, GL_TRUE> const line_smooth;
      gl.hint (GL_LINE_SMOOTH_HINT, GL_NICEST);

      for (auto const& it : model_boxes_to_draw)
      {
        glm::vec4 color = it.first->is_hidden()
                          ? glm::vec4(0.f, 0.f, 1.f, 1.f)
                          : ( it.first->use_fake_geometry()
                              ? glm::vec4(1.f, 0.f, 0.f, 1.f)
                              : glm::vec4(0.75f, 0.75f, 0.75f, 1.f)
                          )
        ;

        m2_box_shader.uniform("color", color);
        it.first->renderer()->drawBox(m2_box_shader, it.second);
      }
    }
    model_boxes_to_draw.clear();

    // Missing-asset placeholders, drawn here and not earlier for two reasons: the state the
    // block above leaves behind is exactly what an opaque cube wants (blending off, depth writes
    // on), and drawing after the real geometry means a placeholder standing inside a building is
    // depth-tested against it rather than painted over it.
    if (!placeholders_to_draw.empty())
    {
      Noggit::Rendering::Primitives::PlaceholderCube::getInstance(_world->_context)
        .draw(projection * model_view, placeholders_to_draw);
    }

    placeholders_to_draw.clear();

    // render m2 selection boxes.
    // TODO can try to move to m2 box shader but it requires some refactor
    if (!render_settings.minimap_render)
    {
      for (auto const& selection : _world->current_selection())
      {
        if (selection.index() == eEntry_Object)
        {
          auto const obj = std::get<selected_object_type>(selection);
      
          if (obj->which() != eMODEL)
            continue;
      
          ModelInstance* model = static_cast<ModelInstance*>(obj);

          // if (model->_rendered_last_frame)
          {
            // bool is_selected = false;
            bool is_selected = _world->is_selected(model->uid);

            bool draw_anim_bb = !(render_settings.draw_models_with_box || model->model->is_hidden());
      
            model->draw_box(model_view, projection, is_selected, render_settings.render_select_m2_collission_bbox
              , render_settings.render_select_m2_aabb, draw_anim_bb);
          }
        }
      }
    }
  }

  // render selection group boxes
  if (!render_settings.minimap_render)
  {
    for (auto const& selection_group : _world->_selection_groups)
    {
        if (!selection_group.isSelected())
            continue;

        glm::mat4x4 identity_mtx = glm::mat4x4{ 1 };
        auto const& extents = selection_group.getExtents();
        Noggit::Rendering::Primitives::WireBox::getInstance(_world->_context).draw(model_view
            , projection
            , identity_mtx
            , { 0.0f, 0.0f, 1.0f, 1.0f } // blue
            , extents[0]
            , extents[1]
        );
    }
  }

  // set anim time only once per frame
  {
    OpenGL::Scoped::use_program water_shader {*_liquid_program.get()};
    water_shader.uniform("camera", glm::vec3(camera_pos.x, camera_pos.y, camera_pos.z));
    water_shader.uniform("animtime", _world->animtime);

    if (render_settings.draw_wmo || _world->mapIndex.hasAGlobalWMO())
    {
      water_shader.uniform("use_transform", 1);
    }
  }
  /*
  // model particles
  if (draw_model_animations && !model_with_particles.empty())
  {
    OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> const cull;
    OpenGL::Scoped::depth_mask_setter<GL_FALSE> const depth_mask;

    OpenGL::Scoped::use_program particles_shader {*_m2_particles_program.get()};

    particles_shader.uniform("model_view_projection", mvp);
    OpenGL::texture::set_active_texture(0);

    for (auto& it : model_with_particles)
    {
      it.first->draw_particles(model_view, particles_shader, it.second);
    }
  }


  if (draw_model_animations && !model_with_particles.empty())
  {
    OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> const cull;
    OpenGL::Scoped::depth_mask_setter<GL_FALSE> const depth_mask;

    OpenGL::Scoped::use_program ribbon_shader {*_m2_ribbons_program.get()};

    ribbon_shader.uniform("model_view_projection", mvp);

    gl.blendFunc(GL_SRC_ALPHA, GL_ONE);

    for (auto& it : model_with_particles)
    {
      it.first->draw_ribbons(ribbon_shader, it.second);
    }
  }

   */

  gl.enable(GL_BLEND);
  gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  // render before the water and enable depth right 
  // so it's visible under water
  // the checker board pattern is used to see the water under it
  if (render_settings.angled_mode || render_settings.use_ref_pos)
  {
    ZoneScopedN("World::draw() : Draw angles");
    // OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> cull;
    OpenGL::Scoped::depth_mask_setter<GL_TRUE> const depth_mask;

    math::degrees orient = math::degrees(render_settings.orientation);
    math::degrees incl = math::degrees(render_settings.angle);
    glm::vec4 color = cursor_color;
    // color.w = 0.5f;
    color.w = 0.75f;

    float radius = 1.2f * render_settings.brush_radius;

    if (render_settings.angled_mode && render_settings.editing_mode == editing_mode::flatten_blur)
    {
      if (render_settings.angle > 49.0f) // 0.855 radian
      {
        color.x = 1.f;
        color.y = 0.f;
        color.z = 0.f;
      }
    }

    if (render_settings.angled_mode && !render_settings.use_ref_pos)
    {
      glm::vec3 pos = cursor_pos;
      pos.y += 0.1f; // to avoid z-fighting with the ground
      _square_render.draw(mvp, pos, radius, incl, orient, color);
    }
    else if (render_settings.use_ref_pos)
    {
      if (render_settings.angled_mode)
      {
        glm::vec3 pos = cursor_pos;
        pos.y = misc::angledHeight(ref_pos, pos, incl, orient);
        pos.y += 0.1f;
        _square_render.draw(mvp, pos, radius, incl, orient, color);

        // display the plane when the cursor is far from ref_point
        if (misc::dist(pos.x, pos.z, ref_pos.x, ref_pos.z) > 10.f + radius)
        {
          glm::vec3 ref = ref_pos;
          ref.y += 0.1f;
          _square_render.draw(mvp, ref, 10.f, incl, orient, color);
        }
      }
      else
      {
        glm::vec3 pos = cursor_pos;
        pos.y = ref_pos.y + 0.1f;
        _square_render.draw(mvp, pos, radius, math::degrees(0.f), math::degrees(0.f), color);
      }
    }
  }

  if (render_settings.draw_water)
  {
    ZoneScopedN("World::draw() : Draw water");

    // draw the water on both sides
    OpenGL::Scoped::bool_setter<GL_CULL_FACE, GL_FALSE> const cull;

    OpenGL::Scoped::use_program water_shader{ *_liquid_program.get()};

    gl.bindVertexArray(_liquid_chunk_vao);

    water_shader.uniform ("use_transform", 0);

    for (auto& pair : _world->_loaded_tiles_buffer)
    {
      MapTile* tile = pair.second;

      if (!tile)
        break;

      if (tile->renderer()->isOccluded() && !tile->Water.needsUpdate() && !tile->renderer()->isOverridingOcclusionCulling())
        continue;

      tile->Water.renderer()->draw(
          frustum
          , camera_pos
          , render_settings.camera_moved
          , water_shader
          , _world->animtime
          , render_settings.water_layer
          , render_settings.display_mode
          , &_liquid_texture_manager
      );
    }

    gl.bindVertexArray(0);
  }

  gl.enable(GL_BLEND);

  // draw last because of the transparency
  if (render_settings.draw_mfbo)
  {
    ZoneScopedN("World::draw() : Draw flight bounds");
    // don't write on the depth buffer
    OpenGL::Scoped::depth_mask_setter<GL_FALSE> const depth_mask;

    OpenGL::Scoped::use_program mfbo_shader {*_mfbo_program.get()};

    for (MapTile* tile : _world->mapIndex.loaded_tiles())
    {
      if (tile->hasFlightBounds())
      {
        tile->flightBoundsRenderer()->draw(mfbo_shader);
      }
    }
  }

  if (render_settings.editing_mode == editing_mode::light && render_settings.alpha_light_sphere > 0.0f)
  {
    // Sky* CurrentSky = skies()->findClosestSkyByDistance(camera_pos);
    // Sky* CurrentSky = skies()->findClosestSkyByWeight();
    // if (!CurrentSky)
    //     return;

    // bad design, there can be multiple current skies, this is only the highest one.
    // all skies we're inside of need to be drawn with front culling
    // int CurrentSkyID = CurrentSky->Id;
        
    const int MAX_TIME_VALUE_C = 2880;
    const int CurrenTime = static_cast<int>(_world->time) % MAX_TIME_VALUE_C;

    // draw Light Zones
    for (auto const& zoneLight : skies()->zoneLightsWotlk)
    {
      Sky* light = skies()->findSkyById(zoneLight.lightId);

      assert(light != nullptr);

      if (glm::distance(light->pos, camera_pos) > (_cull_distance + light->r2) ) // TODO: frustum cull here
        continue;

      glm::vec4 diffuse = { light->colorFor(LIGHT_GLOBAL_DIFFUSE, CurrenTime), 1.f };
      // glm::vec4 ambient = { light->colorFor(LIGHT_GLOBAL_AMBIENT, CurrenTime), 1.f };

      // Render Points
      auto const& zoneLightPoints = zoneLight.points; // skies()->zoneLightPoints[zoneLight.second.id];

      // polygon must have at least 3 points
      if (zoneLightPoints.size() < 3)
        continue;

      std::vector<glm::vec3> lineRenderPoints;

      for (int point_id = 0; point_id < zoneLightPoints.size(); point_id++)
      {
        glm::vec2 const curr_point = zoneLightPoints[point_id];

        // using light z/y pos to set the sphere position, those are supposed to be planes from point to point with infinite height.
        glm::vec3 point_pos = glm::vec3(curr_point.x, light->pos.y, curr_point.y);
        lineRenderPoints.push_back(point_pos);

        // can render a sphere at each point
        // float sphere_radius = 10.f;
        // _sphere_render.draw(mvp, point_pos, diffuse, sphere_radius, 32, 18, alpha_light_sphere, false, false);

        // Connect last point to the first
        if (point_id == (zoneLightPoints.size() - 1))
        {
          lineRenderPoints.push_back(lineRenderPoints[0]);
        }
      }
      _line_render.draw(mvp, lineRenderPoints, diffuse, false); // glm::vec4(1.f, 0.f, 0.f, 1.f) red

      // debug testing, only render first zone
      // break;

      // TODO render a vertical rectangle between each points to draw the polygon in 3D
    }

    // Draw Sky/Light spheres
    glCullFace(GL_FRONT);
    if (!render_settings.draw_only_inside_light_sphere)
    {
      for (Sky const& sky : skies()->skies)
      {
        // we draw skies we're inside of later with glCullFace(GL_BACK);
        if (/*CurrentSkyID == sky.Id || */sky.weight > 0.0f || sky.global)
          continue;

        if (glm::distance(sky.pos, camera_pos) <= _cull_distance) // TODO: frustum cull here
        {
          glm::vec4 diffuse = { sky.colorFor(LIGHT_GLOBAL_DIFFUSE, CurrenTime), 1.0f };
          glm::vec4 ambient = { sky.colorFor(LIGHT_GLOBAL_AMBIENT, CurrenTime), 1.0f };

          _sphere_render.draw(mvp, sky.pos, ambient, sky.r1, 32, 18
            , render_settings.alpha_light_sphere, false, render_settings.draw_wireframe_light_sphere);
          _sphere_render.draw(mvp, sky.pos, diffuse, sky.r2, 32, 18
            , render_settings.alpha_light_sphere, false, render_settings.draw_wireframe_light_sphere);
        
          // special wirebox to highlight zone lights
          if (sky.zone_light)
          {
            glm::vec3 minExtent =  glm::vec3(sky.pos.x - sky.r2, sky.pos.y - sky.r2, sky.pos.z - sky.r2);
            glm::vec3 maxExtent = glm::vec3(sky.pos.x + sky.r2, sky.pos.y + sky.r2, sky.pos.z + sky.r2);

            _wirebox_render.draw(model_view, projection, glm::mat4x4{ 1 }, { 1.0f, 1.0f, 1.0f, 1.0f },
                        minExtent, maxExtent);
          }

          // TODO Those lines tank fps by 50%
          // std::vector<glm::vec3> linePoints;
          // linePoints.push_back(glm::vec3(sky.pos.x, sky.pos.y, sky.pos.z - sky.r2));
          // linePoints.push_back(glm::vec3(sky.pos.x, sky.pos.y, sky.pos.z + sky.r2));
          // _line_render.draw(mvp, linePoints, glm::vec4(1.f), false);
        }
      }
    }

    // now draw the current light (light that we're inside of)
    glCullFace(GL_BACK);
    for (Sky const& sky : skies()->skies)
    {
      if (sky.global)
        continue;
      if (/*CurrentSky->getId() == sky.Id ||*/ sky.weight > 0.0f)
      {
        glm::vec4 diffuse = { sky.colorFor(LIGHT_GLOBAL_DIFFUSE, CurrenTime), 1.0f };
        glm::vec4 ambient = { sky.colorFor(LIGHT_GLOBAL_AMBIENT, CurrenTime), 1.0f };

        // always render wireframe in the current light
        // need to render outer first or it gets culled
        _sphere_render.draw(mvp, sky.pos, diffuse, sky.r2, 32, 18
          , render_settings.alpha_light_sphere, true, false);
        _sphere_render.draw(mvp, sky.pos, ambient, sky.r1, 32, 18
          , render_settings.alpha_light_sphere, true, false);


        // std::vector<glm::vec3> linePoints;
        // linePoints.push_back(glm::vec3(CurrentSky->pos.x, CurrentSky->pos.z, CurrentSky->pos.y - CurrentSky->r2));
        // linePoints.push_back(glm::vec3(CurrentSky->pos.x, CurrentSky->pos.z, CurrentSky->pos.y + CurrentSky->r2));
        // _line_render.draw(mvp, linePoints, glm::vec4(1.f, 0.f, 0.f, 1.f), false);
      }
    }
  }
}

void WorldRender::upload()
{
  ZoneScoped;

  if (_world->mapIndex.hasAGlobalWMO())
  {
    WMOInstance inst(_world->mWmoFilename, &_world->mWmoEntry, _world->_context);

    _world->_model_instance_storage.add_wmo_instance(std::move(inst), false, false);
  }
  else
  {
    _horizon_render = std::make_unique<Noggit::map_horizon::render>(_world->horizon);
  }

  _skies = std::make_unique<Skies>(_world->mapIndex._map_id, _world->_context);

  _outdoor_lighting = std::make_unique<OutdoorLighting>();

  _m2_program.reset
    ( new OpenGL::program
          { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("m2_vs") }
              , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("m2_fs") }
          }
    );

  _m2_instanced_program.reset
      ( new OpenGL::program
            { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("m2_vs", {"instanced"}) }
                , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("m2_fs") }
            }
      );

  _m2_box_program.reset
      ( new OpenGL::program
            { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("m2_box_vs") }
                , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("m2_box_fs") }
            }
      );

  _m2_ribbons_program.reset
      ( new OpenGL::program
            { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("ribbon_vs") }
                , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("ribbon_fs") }
            }
      );

  _m2_particles_program.reset
      ( new OpenGL::program
            { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("particle_vs") }
                , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("particle_fs") }
            }
      );

  _mcnk_program.reset
      ( new OpenGL::program
            { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("terrain_vs") }
                , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("terrain_fs") }
            }
      );

  _mfbo_program.reset
      ( new OpenGL::program
            { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("mfbo_vs") }
                , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("mfbo_fs") }
            }
      );

  _wmo_program.reset
      ( new OpenGL::program
            { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("wmo_vs") }
                , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("wmo_fs") }
            }
      );

  _liquid_program.reset(
      new OpenGL::program
          { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("liquid_vs") }
              , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("liquid_fs") }
          }
  );

  _occluder_program.reset(
      new OpenGL::program
          { { GL_VERTEX_SHADER,   OpenGL::shader::src_from_qrc("occluder_vs") }
              , { GL_FRAGMENT_SHADER, OpenGL::shader::src_from_qrc("occluder_fs") }
          }
  );

  _liquid_texture_manager.upload();

  _buffers.upload();
  _vertex_arrays.upload();

  setupOccluderBuffers();

  {
    OpenGL::Scoped::use_program m2_shader {*_m2_program.get()};
    m2_shader.uniform("bone_matrices", 0);
    m2_shader.uniform("tex1", 1);
    m2_shader.uniform("tex2", 2);

    m2_shader.bind_uniform_block("matrices", 0);
    gl.bindBuffer(GL_UNIFORM_BUFFER, _mvp_ubo);
    gl.bufferData(GL_UNIFORM_BUFFER, sizeof(OpenGL::MVPUniformBlock), NULL, GL_DYNAMIC_DRAW);
    gl.bindBufferRange(GL_UNIFORM_BUFFER, OpenGL::ubo_targets::MVP, _mvp_ubo, 0, sizeof(OpenGL::MVPUniformBlock));
    gl.bindBuffer(GL_UNIFORM_BUFFER, 0);

    m2_shader.bind_uniform_block("lighting", 1);
    gl.bindBuffer(GL_UNIFORM_BUFFER, _lighting_ubo);
    gl.bufferData(GL_UNIFORM_BUFFER, sizeof(OpenGL::LightingUniformBlock), NULL, GL_DYNAMIC_DRAW);
    gl.bindBufferRange(GL_UNIFORM_BUFFER, OpenGL::ubo_targets::LIGHTING, _lighting_ubo, 0, sizeof(OpenGL::LightingUniformBlock));
    gl.bindBuffer(GL_UNIFORM_BUFFER, 0);
  }

  {
    std::vector<int> samplers {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

    OpenGL::Scoped::use_program wmo_program {*_wmo_program.get()};
    wmo_program.uniform("render_batches_tex", 0);
    wmo_program.uniform("texture_samplers", samplers);
    wmo_program.bind_uniform_block("matrices", 0);
    wmo_program.bind_uniform_block("lighting", 1);
  }

  {
    OpenGL::Scoped::use_program mcnk_shader {*_mcnk_program.get()};

    setupChunkBuffers();
    setupChunkVAO(mcnk_shader);

    mcnk_shader.bind_uniform_block("matrices", 0);
    mcnk_shader.bind_uniform_block("lighting", 1);
    mcnk_shader.bind_uniform_block("overlay_params", 2);
    mcnk_shader.bind_uniform_block("chunk_instances", 3);

    gl.bindBuffer(GL_UNIFORM_BUFFER, _terrain_params_ubo);
    gl.bufferData(GL_UNIFORM_BUFFER, sizeof(OpenGL::TerrainParamsUniformBlock), NULL, GL_STATIC_DRAW);
    gl.bindBufferRange(GL_UNIFORM_BUFFER, OpenGL::ubo_targets::TERRAIN_OVERLAYS, _terrain_params_ubo, 0, sizeof(OpenGL::TerrainParamsUniformBlock));
    gl.bindBuffer(GL_UNIFORM_BUFFER, 0);

    mcnk_shader.uniform("heightmap", 0);
    mcnk_shader.uniform("mccv", 1);
    mcnk_shader.uniform("shadowmap", 2);
    mcnk_shader.uniform("alphamap", 3);
    mcnk_shader.uniform("stamp_brush", 4);
    mcnk_shader.uniform("base_instance", 0);

    std::vector<int> samplers {5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    mcnk_shader.uniform("textures", samplers);

  }

  {
    OpenGL::Scoped::use_program m2_shader_instanced {*_m2_instanced_program.get()};
    m2_shader_instanced.bind_uniform_block("matrices", 0);
    m2_shader_instanced.bind_uniform_block("lighting", 1);
    m2_shader_instanced.uniform("bone_matrices", 0);
    m2_shader_instanced.uniform("tex1", 1);
    m2_shader_instanced.uniform("tex2", 2);
  }

  /*
  {
    OpenGL::Scoped::use_program particles_shader {*_m2_particles_program.get()};
    particles_shader.uniform("tex", 0);
  }

  {
    OpenGL::Scoped::use_program ribbon_shader {*_m2_ribbons_program.get()};
    ribbon_shader.uniform("tex", 0);
  }

   */

  {
    OpenGL::Scoped::use_program liquid_render {*_liquid_program.get()};

    setupLiquidChunkBuffers();
    setupLiquidChunkVAO(liquid_render);

    static std::vector<int> samplers {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

    liquid_render.bind_uniform_block("matrices", 0);
    liquid_render.bind_uniform_block("lighting", 1);
    liquid_render.bind_uniform_block("liquid_layers_params", 4);
    liquid_render.uniform("vertex_data", 0);
    liquid_render.uniform("texture_samplers", samplers);

  }

  {
    OpenGL::Scoped::use_program mfbo_shader {*_mfbo_program.get()};
    mfbo_shader.bind_uniform_block("matrices", 0);
  }

  {
    OpenGL::Scoped::use_program m2_box_shader {*_m2_box_program.get()};
    m2_box_shader.bind_uniform_block("matrices", 0);
  }

  {
    OpenGL::Scoped::use_program occluder_shader {*_occluder_program.get()};
    occluder_shader.bind_uniform_block("matrices", 0);
  }


}

void WorldRender::unload()
{
  ZoneScoped;
  _mcnk_program.reset();
  _mfbo_program.reset();
  _m2_program.reset();
  _m2_instanced_program.reset();
  _m2_particles_program.reset();
  _m2_ribbons_program.reset();
  _m2_box_program.reset();
  _wmo_program.reset();
  _liquid_program.reset();

  _cursor_render.unload();
  _sphere_render.unload();
  _square_render.unload();
  _line_render.unload();
  _wirebox_render.unload();

  _horizon_render.reset();

  _liquid_texture_manager.unload();

  // Before the shader and buffer teardown below only because it is the one member here that holds
  // model references: clearing it can run ~Model, which needs the context this function is called
  // inside. The object itself stays alive so the user's preview settings survive an unload.
  _detail_doodad_render->clear();

  _skies->unload();

  _buffers.unload();
  _vertex_arrays.unload();

  Noggit::Rendering::Primitives::WireBox::getInstance(_world->_context).unload();
  Noggit::Rendering::Primitives::PlaceholderCube::getInstance(_world->_context).unload();
}


void WorldRender::updateMVPUniformBlock(const glm::mat4x4& model_view, const glm::mat4x4& projection)
{
  ZoneScoped;

  _mvp_ubo_data.model_view = model_view;
  _mvp_ubo_data.projection = projection;

  gl.bindBuffer(GL_UNIFORM_BUFFER, _mvp_ubo);
  gl.bufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(OpenGL::MVPUniformBlock), &_mvp_ubo_data);

}

void WorldRender::updateLightingUniformBlock(bool draw_fog, glm::vec3 const& camera_pos)
{
  ZoneScoped;

  _outdoor_light_stats = _outdoor_lighting->getLightStats(static_cast<int>(_world->time));

  glm::vec3 diffuse = _skies->color_set[LIGHT_GLOBAL_DIFFUSE];
  glm::vec3 ambient = _skies->color_set[LIGHT_GLOBAL_AMBIENT];
  glm::vec3 fog_color = _skies->color_set[SKY_FOG_COLOR];
  glm::vec3 ocean_color_light = _skies->color_set[OCEAN_COLOR_LIGHT];
  glm::vec3 ocean_color_dark = _skies->color_set[OCEAN_COLOR_DARK];
  glm::vec3 river_color_light = _skies->color_set[RIVER_COLOR_LIGHT];
  glm::vec3 river_color_dark = _skies->color_set[RIVER_COLOR_DARK];


  _lighting_ubo_data.DiffuseColor_FogStart = {diffuse.x,diffuse.y,diffuse.z, _skies->fog_distance_start()};
  _lighting_ubo_data.AmbientColor_FogEnd = {ambient.x,ambient.y,ambient.z, _skies->fog_distance_end()};
  _lighting_ubo_data.FogColor_FogOn = {fog_color.x,fog_color.y,fog_color.z, static_cast<float>(draw_fog)};

  if (directional_lightning)
    _lighting_ubo_data.LightDir_FogRate = { _outdoor_light_stats.dayDir.x, _outdoor_light_stats.dayDir.y, _outdoor_light_stats.dayDir.z, _skies->fogRate() };
  else
    _lighting_ubo_data.LightDir_FogRate = {0.0f, -1.0f, 0.0f, _skies->fogRate()};

  _lighting_ubo_data.OceanColorLight = { ocean_color_light.x,ocean_color_light.y,ocean_color_light.z, _skies->ocean_shallow_alpha()};
  _lighting_ubo_data.OceanColorDark = { ocean_color_dark.x,ocean_color_dark.y,ocean_color_dark.z, _skies->ocean_deep_alpha()};
  _lighting_ubo_data.RiverColorLight = { river_color_light.x,river_color_light.y,river_color_light.z, _skies->river_shallow_alpha()};
  _lighting_ubo_data.RiverColorDark = { river_color_dark.x,river_color_dark.y,river_color_dark.z, _skies->river_deep_alpha()};

  gl.bindBuffer(GL_UNIFORM_BUFFER, _lighting_ubo);
  gl.bufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(OpenGL::LightingUniformBlock), &_lighting_ubo_data);
}

void WorldRender::updateLightingUniformBlockMinimap(MinimapRenderSettings* settings)
{
  ZoneScoped;

  glm::vec3 diffuse = settings->diffuse_color;
  glm::vec3 ambient = settings->ambient_color;

  _lighting_ubo_data.FogColor_FogOn = { 0, 0, 0, 0 };
  if (settings->export_mode == MinimapGenMode::LOD_MAPTEXTURES) {
      _lighting_ubo_data.DiffuseColor_FogStart = { 0.5, 0.5, 0.5, 0 };
      _lighting_ubo_data.AmbientColor_FogEnd = { 0.5, 0.5, 0.5, 0 };
      _lighting_ubo_data.LightDir_FogRate = { 0.0, -1.0, 0.0, _skies->fogRate() };
  }
  else {
      _lighting_ubo_data.DiffuseColor_FogStart = { diffuse, 0 };
      _lighting_ubo_data.AmbientColor_FogEnd = { ambient, 0 };
      _lighting_ubo_data.LightDir_FogRate = { _outdoor_light_stats.dayDir.x, _outdoor_light_stats.dayDir.y, _outdoor_light_stats.dayDir.z, _skies->fogRate() };
  }
  _lighting_ubo_data.OceanColorLight = settings->ocean_color_light;
  _lighting_ubo_data.OceanColorDark = settings->ocean_color_dark;
  _lighting_ubo_data.RiverColorLight = settings->river_color_light;
  _lighting_ubo_data.RiverColorDark = settings->river_color_dark;

  gl.bindBuffer(GL_UNIFORM_BUFFER, _lighting_ubo);
  gl.bufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(OpenGL::LightingUniformBlock), &_lighting_ubo_data);
}

void WorldRender::updateTerrainParamsUniformBlock()
{
  ZoneScoped;
  gl.bindBuffer(GL_UNIFORM_BUFFER, _terrain_params_ubo);
  gl.bufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(OpenGL::TerrainParamsUniformBlock), &_terrain_params_ubo_data);
  _need_terrain_params_ubo_update = false;
}

void Noggit::Rendering::WorldRender::markTerrainParamsUniformBlockDirty()
{
  _need_terrain_params_ubo_update = true;
}

[[nodiscard]]
std::unique_ptr<Skies>& Noggit::Rendering::WorldRender::skies()
{
  return _skies;
}

float Noggit::Rendering::WorldRender::cullDistance() const
{
  return _cull_distance;
}

Noggit::Rendering::DetailDoodadRender& Noggit::Rendering::WorldRender::detailDoodads()
{
  return *_detail_doodad_render;
}

void WorldRender::setupChunkVAO(OpenGL::Scoped::use_program& mcnk_shader)
{
  ZoneScoped;
  OpenGL::Scoped::vao_binder const _ (_mapchunk_vao);

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const binder(_mapchunk_texcoord);
    mcnk_shader.attrib("texcoord", 2, GL_FLOAT, GL_FALSE, 0, 0);
  }

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const binder(_mapchunk_vertex);
    mcnk_shader.attrib("position", 2, GL_FLOAT, GL_FALSE, 0, 0);
  }
}

void WorldRender::setupChunkBuffers()
{
  ZoneScoped;

  // vertices

  glm::vec2 vertices[mapbufsize];
  glm::vec2 *ttv = vertices;

  for (int j = 0; j < 17; ++j)
  {
    bool is_lod = j % 2;
    for (int i = 0; i < (is_lod ? 8 : 9); ++i)
    {
      float xpos, zpos;
      xpos = i * UNITSIZE;
      zpos = j * 0.5f * UNITSIZE;

      if (is_lod)
      {
        xpos += UNITSIZE*0.5f;
      }

      auto v = glm::vec2(xpos, zpos);
      *ttv++ = v;
    }
  }

  gl.bufferData<GL_ARRAY_BUFFER>(_mapchunk_vertex, sizeof(vertices), vertices, GL_STATIC_DRAW);


  static constexpr std::array<std::uint16_t, 768 + 192> indices {

      9, 0, 17, 9, 17, 18, 9, 18, 1, 9, 1, 0, 26, 17, 34, 26,
      34, 35, 26, 35, 18, 26, 18, 17, 43, 34, 51, 43, 51, 52, 43, 52,
      35, 43, 35, 34, 60, 51, 68, 60, 68, 69, 60, 69, 52, 60, 52, 51,
      77, 68, 85, 77, 85, 86, 77, 86, 69, 77, 69, 68, 94, 85, 102, 94,
      102, 103, 94, 103, 86, 94, 86, 85, 111, 102, 119, 111, 119, 120, 111, 120,
      103, 111, 103, 102, 128, 119, 136, 128, 136, 137, 128, 137, 120, 128, 120, 119,
      10, 1, 18, 10, 18, 19, 10, 19, 2, 10, 2, 1, 27, 18, 35, 27,
      35, 36, 27, 36, 19, 27, 19, 18, 44, 35, 52, 44, 52, 53, 44, 53,
      36, 44, 36, 35, 61, 52, 69, 61, 69, 70, 61, 70, 53, 61, 53, 52,
      78, 69, 86, 78, 86, 87, 78, 87, 70, 78, 70, 69, 95, 86, 103, 95,
      103, 104, 95, 104, 87, 95, 87, 86, 112, 103, 120, 112, 120, 121, 112, 121,
      104, 112, 104, 103, 129, 120, 137, 129, 137, 138, 129, 138, 121, 129, 121, 120,
      11, 2, 19, 11, 19, 20, 11, 20, 3, 11, 3, 2, 28, 19, 36, 28,
      36, 37, 28, 37, 20, 28, 20, 19, 45, 36, 53, 45, 53, 54, 45, 54,
      37, 45, 37, 36, 62, 53, 70, 62, 70, 71, 62, 71, 54, 62, 54, 53,
      79, 70, 87, 79, 87, 88, 79, 88, 71, 79, 71, 70, 96, 87, 104, 96,
      104, 105, 96, 105, 88, 96, 88, 87, 113, 104, 121, 113, 121, 122, 113, 122,
      105, 113, 105, 104, 130, 121, 138, 130, 138, 139, 130, 139, 122, 130, 122, 121,
      12, 3, 20, 12, 20, 21, 12, 21, 4, 12, 4, 3, 29, 20, 37, 29,
      37, 38, 29, 38, 21, 29, 21, 20, 46, 37, 54, 46, 54, 55, 46, 55,
      38, 46, 38, 37, 63, 54, 71, 63, 71, 72, 63, 72, 55, 63, 55, 54,
      80, 71, 88, 80, 88, 89, 80, 89, 72, 80, 72, 71, 97, 88, 105, 97,
      105, 106, 97, 106, 89, 97, 89, 88, 114, 105, 122, 114, 122, 123, 114, 123,
      106, 114, 106, 105, 131, 122, 139, 131, 139, 140, 131, 140, 123, 131, 123, 122,
      13, 4, 21, 13, 21, 22, 13, 22, 5, 13, 5, 4, 30, 21, 38, 30,
      38, 39, 30, 39, 22, 30, 22, 21, 47, 38, 55, 47, 55, 56, 47, 56,
      39, 47, 39, 38, 64, 55, 72, 64, 72, 73, 64, 73, 56, 64, 56, 55,
      81, 72, 89, 81, 89, 90, 81, 90, 73, 81, 73, 72, 98, 89, 106, 98,
      106, 107, 98, 107, 90, 98, 90, 89, 115, 106, 123, 115, 123, 124, 115, 124,
      107, 115, 107, 106, 132, 123, 140, 132, 140, 141, 132, 141, 124, 132, 124, 123,
      14, 5, 22, 14, 22, 23, 14, 23, 6, 14, 6, 5, 31, 22, 39, 31,
      39, 40, 31, 40, 23, 31, 23, 22, 48, 39, 56, 48, 56, 57, 48, 57,
      40, 48, 40, 39, 65, 56, 73, 65, 73, 74, 65, 74, 57, 65, 57, 56,
      82, 73, 90, 82, 90, 91, 82, 91, 74, 82, 74, 73, 99, 90, 107, 99,
      107, 108, 99, 108, 91, 99, 91, 90, 116, 107, 124, 116, 124, 125, 116, 125,
      108, 116, 108, 107, 133, 124, 141, 133, 141, 142, 133, 142, 125, 133, 125, 124,
      15, 6, 23, 15, 23, 24, 15, 24, 7, 15, 7, 6, 32, 23, 40, 32,
      40, 41, 32, 41, 24, 32, 24, 23, 49, 40, 57, 49, 57, 58, 49, 58,
      41, 49, 41, 40, 66, 57, 74, 66, 74, 75, 66, 75, 58, 66, 58, 57,
      83, 74, 91, 83, 91, 92, 83, 92, 75, 83, 75, 74, 100, 91, 108, 100,
      108, 109, 100, 109, 92, 100, 92, 91, 117, 108, 125, 117, 125, 126, 117, 126,
      109, 117, 109, 108, 134, 125, 142, 134, 142, 143, 134, 143, 126, 134, 126, 125,
      16, 7, 24, 16, 24, 25, 16, 25, 8, 16, 8, 7, 33, 24, 41, 33,
      41, 42, 33, 42, 25, 33, 25, 24, 50, 41, 58, 50, 58, 59, 50, 59,
      42, 50, 42, 41, 67, 58, 75, 67, 75, 76, 67, 76, 59, 67, 59, 58,
      84, 75, 92, 84, 92, 93, 84, 93, 76, 84, 76, 75, 101, 92, 109, 101,
      109, 110, 101, 110, 93, 101, 93, 92, 118, 109, 126, 118, 126, 127, 118, 127,
      110, 118, 110, 109, 135, 126, 143, 135, 143, 144, 135, 144, 127, 135, 127, 126,

      // lod
      0, 34, 18, 18, 34, 36, 18, 36, 2, 18, 2, 0, 34, 68, 52, 52,
      68, 70, 52, 70, 36, 52, 36, 34, 68, 102, 86, 86, 102, 104, 86, 104,
      70, 86, 70, 68, 102, 136, 120, 120, 136, 138, 120, 138, 104, 120, 104, 102,
      2, 36, 20, 20, 36, 38, 20, 38, 4, 20, 4, 2, 36, 70, 54, 54,
      70, 72, 54, 72, 38, 54, 38, 36, 70, 104, 88, 88, 104, 106, 88, 106,
      72, 88, 72, 70, 104, 138, 122, 122, 138, 140, 122, 140, 106, 122, 106, 104,
      4, 38, 22, 22, 38, 40, 22, 40, 6, 22, 6, 4, 38, 72, 56, 56,
      72, 74, 56, 74, 40, 56, 40, 38, 72, 106, 90, 90, 106, 108, 90, 108,
      74, 90, 74, 72, 106, 140, 124, 124, 140, 142, 124, 142, 108, 124, 108, 106,
      6, 40, 24, 24, 40, 42, 24, 42, 8, 24, 8, 6, 40, 74, 58, 58,
      74, 76, 58, 76, 42, 58, 42, 40, 74, 108, 92, 92, 108, 110, 92, 110,
      76, 92, 76, 74, 108, 142, 126, 126, 142, 144, 126, 144, 110, 126, 110, 108};

  /*
  // indices
  std::uint16_t indices[768];
  int flat_index = 0;

  for (int x = 0; x<8; ++x)
  {
    for (int y = 0; y<8; ++y)
    {
      indices[flat_index++] = MapChunk::indexLoD(y, x); //9
      indices[flat_index++] = MapChunk::indexNoLoD(y, x); //0
      indices[flat_index++] = MapChunk::indexNoLoD(y + 1, x); //17
      indices[flat_index++] = MapChunk::indexLoD(y, x); //9
      indices[flat_index++] = MapChunk::indexNoLoD(y + 1, x); //17
      indices[flat_index++] = MapChunk::indexNoLoD(y + 1, x + 1); //18
      indices[flat_index++] = MapChunk::indexLoD(y, x); //9
      indices[flat_index++] = MapChunk::indexNoLoD(y + 1, x + 1); //18
      indices[flat_index++] = MapChunk::indexNoLoD(y, x + 1); //1
      indices[flat_index++] = MapChunk::indexLoD(y, x); //9
      indices[flat_index++] = MapChunk::indexNoLoD(y, x + 1); //1
      indices[flat_index++] = MapChunk::indexNoLoD(y, x); //0
    }
  }

   */

  {
    OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> const _ (_mapchunk_index);
    gl.bufferData (GL_ELEMENT_ARRAY_BUFFER, (768 + 192) * sizeof(std::uint16_t), indices.data(), GL_STATIC_DRAW);
  }

  // tex coords
  glm::vec2 temp[mapbufsize], *vt;
  float tx, ty;

  // init texture coordinates for detail map:
  vt = temp;
  const float detail_half = 0.5f * detail_size / 8.0f;
  for (int j = 0; j < 17; ++j)
  {
    bool is_lod = j % 2;

    for (int i = 0; i< (is_lod ? 8 : 9); ++i)
    {
      tx = detail_size / 8.0f * i;
      ty = detail_size / 8.0f * j * 0.5f;

      if (is_lod)
        tx += detail_half;

      *vt++ = glm::vec2(tx, ty);
    }
  }

  gl.bufferData<GL_ARRAY_BUFFER> (_mapchunk_texcoord, sizeof(temp), temp, GL_STATIC_DRAW);

}

void WorldRender::setupLiquidChunkVAO(OpenGL::Scoped::use_program& water_shader)
{
  ZoneScoped;
  OpenGL::Scoped::vao_binder const _ (_liquid_chunk_vao);

  {
    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> const binder(_liquid_chunk_vertex);
    water_shader.attrib("position", 2, GL_FLOAT, GL_FALSE, 0, 0);
  }
}

void WorldRender::setupLiquidChunkBuffers()
{
  ZoneScoped;

  // vertices
  glm::vec2 vertices[768 / 2];
  glm::vec2* vt = vertices;

  for (int z = 0; z < 8; ++z)
  {
    for (int x = 0; x < 8; ++x)
    {
      // first triangle
      *vt++ = glm::vec2(UNITSIZE * x, UNITSIZE * z);
      *vt++ = glm::vec2(UNITSIZE * x, UNITSIZE * (z + 1));
      *vt++ = glm::vec2(UNITSIZE * (x + 1), UNITSIZE * z);

      // second triangle
      *vt++ = glm::vec2(UNITSIZE * (x + 1), UNITSIZE * z);
      *vt++ = glm::vec2(UNITSIZE * x, UNITSIZE * (z + 1));
      *vt++ = glm::vec2(UNITSIZE * (x + 1), UNITSIZE * (z + 1));
    }
  }

  gl.bufferData<GL_ARRAY_BUFFER> (_liquid_chunk_vertex, sizeof(vertices), vertices, GL_STATIC_DRAW);

}



void WorldRender::setupOccluderBuffers()
{
  ZoneScoped;
  static constexpr std::array<std::uint16_t, 36> indices
      {
          /*Above ABC,BCD*/
          0,1,2,
          1,2,3,
          /*Following EFG,FGH*/
          4,5,6,
          5,6,7,
          /*Left ABF,AEF*/
          1,0,5,
          0,4,5,
          /*Right side CDH,CGH*/
          3,2,7,
          2,6,7,
          /*ACG,AEG*/
          2,0,6,
          0,4,6,
          /*Behind BFH,BDH*/
          5,1,7,
          1,3,7
      };

  {
    OpenGL::Scoped::buffer_binder<GL_ELEMENT_ARRAY_BUFFER> const _ (_occluder_index);
    gl.bufferData (GL_ELEMENT_ARRAY_BUFFER, 36 * sizeof(std::uint16_t), indices.data(), GL_STATIC_DRAW);
  }

}

void WorldRender::drawMinimap ( MapTile *tile
    , glm::mat4x4 const& model_view
    , glm::mat4x4 const& projection
    , glm::vec3 const& camera_pos
    , MinimapRenderSettings* settings
)
{
  ZoneScoped;

  // Also load a tile above the current one to correct the lookat approximation
  TileIndex m_tile = TileIndex(camera_pos);
  m_tile.z -= 1;

  bool unload = !_world->mapIndex.has_unsaved_changes(m_tile);

  MapTile* mTile = _world->mapIndex.loadTile(m_tile);

  if (mTile)
  {
    mTile->wait_until_loaded();
    mTile->waitForChildrenLoaded();

  }

  WorldRenderParams renderParams;

  renderParams.cursorRotation = 0.0f;
  renderParams.cursor_type = CursorType::NONE;
  renderParams.brush_radius = 0.f;
  renderParams.show_unpaintable_chunks = false;
  renderParams.draw_only_inside_light_sphere = false;
  renderParams.draw_wireframe_light_sphere = false;
  renderParams.alpha_light_sphere = false;
  renderParams.inner_radius_ratio = 0.3f;
  renderParams.angle = 0.0f;
  renderParams.orientation = 0.0f;
  renderParams.use_ref_pos = 0.0f;
  renderParams.angled_mode = 0.0f;
  renderParams.draw_paintability_overlay = false;
  renderParams.editing_mode = editing_mode::minimap;
  renderParams.camera_moved = true;
  renderParams.draw_mfbo = false;
  renderParams.draw_terrain = true;
  renderParams.draw_wmo = settings->draw_wmo;
  renderParams.draw_water = settings->draw_water;
  renderParams.draw_wmo_doodads = false;
  renderParams.draw_models = settings->draw_m2;
  renderParams.draw_model_animations = false;
  renderParams.draw_models_with_box = false;
  renderParams.draw_hidden_models = true;
  renderParams.draw_sky = false;
  renderParams.draw_skybox = false;
  renderParams.draw_fog = false;
  renderParams.ground_editing_brush = eTerrainType::eTerrainType_Linear;
  renderParams.water_layer = 0;
  renderParams.display_mode = display_mode::in_3D;
  renderParams.draw_occlusion_boxes = false;
  renderParams.minimap_render = true;
  renderParams.draw_wmo_exterior = true;

  draw(model_view, projection, glm::vec3(), glm::vec4(),
  glm::vec3(), camera_pos, settings, renderParams);


  if (unload)
  {
    _world->mapIndex.unloadTile(m_tile);
  }
}

bool WorldRender::renderSunDepth( TileIndex const& tile_idx
                               , Noggit::Rendering::ShadowBakeSettings const& settings
                               , Noggit::Rendering::SunDepthMap& out
                               , int* out_neighbour_tiles_loaded
                               )
{
  ZoneScoped;

  out = Noggit::Rendering::SunDepthMap();

  Noggit::Rendering::ShadowBakeSettings const clean (settings.sanitized());

  MapTile* tile = _world->mapIndex.getTile(tile_idx);

  if (!tile || !tile->finishedLoading())
  {
    return false;
  }

  // The volume the bake has to cover: this tile's footprint, and its own vertical extent rather
  // than a guessed constant. makeSunTransform grows it by the caster margin on every side, so
  // nothing here has to anticipate which way the light is coming from.
  //
  // getExtents() and not getCombinedExtents(): the box being fitted is the ground that will
  // RECEIVE shadow, which is terrain only. Objects standing on it are casters and are covered by
  // the caster margin instead -- folding a 300-yard WMO's bounding box into the receiver volume
  // would stretch the orthographic box and cost depth resolution over the whole tile to no
  // purpose.
  auto const& extents = tile->getExtents();

  glm::vec3 const min_bounds (tile->xbase, extents[0].y, tile->zbase);
  glm::vec3 const max_bounds ( tile->xbase + TILESIZE
                             , extents[1].y
                             , tile->zbase + TILESIZE
                             );

  if (!Noggit::Rendering::makeSunTransform(min_bounds, max_bounds, clean, out))
  {
    return false;
  }

  int const resolution = clean.depth_resolution;

  // Colour attachment as well as depth, even though only depth is read back. A framebuffer with
  // no colour buffer is legal but the terrain, M2 and WMO programs all write a colour output and
  // several of them blend; dropping the attachment would make every one of those writes undefined
  // behaviour to no benefit, since the depth attachment is the same size either way.
  QOpenGLFramebufferObjectFormat fmt;
  fmt.setSamples(0);
  fmt.setInternalTextureFormat(GL_RGBA8);
  fmt.setAttachment(QOpenGLFramebufferObject::Depth);

  QOpenGLFramebufferObject pixel_buffer(resolution, resolution, fmt);

  if (!pixel_buffer.isValid())
  {
    // A 8192-square renderbuffer is past what some GL 4.1 drivers will allocate. Failing here and
    // saying so beats rendering into a zero-sized target and reporting a bake that shadowed
    // nothing.
    return false;
  }

  // Saved before the FBO is bound, because the viewport is CONTEXT state and setting it below
  // would otherwise be a one-way change.
  //
  // MapView sets the viewport in resizeGL and nowhere else (MapView.cpp:5256-5259) -- there is no
  // per-frame glViewport in paintGL or in draw(). Qt's QOpenGLWidget is understood to set it
  // itself before each invokeUserPaint, which is presumably why saveMinimap has got away with
  // leaving a 512-square viewport behind since it was written; but "presumably" is not a thing to
  // stake the whole viewport on, and if that understanding is wrong the symptom is the editor
  // rendering at the bake's resolution into a window-sized buffer until the user happens to
  // resize the window. Four integers is a cheap way not to depend on it.
  GLint saved_viewport[4] = {0, 0, 0, 0};
  gl.getIntegerv(GL_VIEWPORT, saved_viewport);

  pixel_buffer.bind();

  gl.viewport(0, 0, resolution, resolution);
  gl.clearColor(0.0f, 0.0f, 0.0f, 1.0f);

  // Depth writes have to be enabled for the clear to reach the depth buffer at all. draw() leaves
  // the mask in whatever state its last pass set (ModelRender.cpp:896-906 toggles it per render
  // flag), so this cannot be assumed.
  gl.depthMask(GL_TRUE);
  gl.clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  // Default-constructed and never shown to the user. draw() dereferences this unconditionally
  // whenever minimap_render is set (WorldRender.cpp:90 and :96), and its filter list pointers are
  // only touched behind use_filters, which defaults false -- so a default instance is safe and is
  // the smallest thing that satisfies the contract.
  MinimapRenderSettings depth_pass_settings;
  depth_pass_settings.draw_shadows = false;
  depth_pass_settings.draw_water = false;
  depth_pass_settings.use_filters = false;

  WorldRenderParams render_params;

  render_params.cursorRotation = 0.0f;
  render_params.cursor_type = CursorType::NONE;
  render_params.brush_radius = 0.0f;
  render_params.show_unpaintable_chunks = false;
  render_params.draw_only_inside_light_sphere = false;
  render_params.draw_wireframe_light_sphere = false;
  render_params.alpha_light_sphere = 0.0f;
  render_params.inner_radius_ratio = 0.0f;
  render_params.angle = 0.0f;
  render_params.orientation = 0.0f;
  render_params.use_ref_pos = false;
  render_params.angled_mode = false;
  render_params.draw_paintability_overlay = false;

  // NOT editing_mode::minimap. draw() reads minimap_render_settings->selected_tiles only when the
  // editing mode is minimap (WorldRender.cpp:355-356), and a shadow bake must never be filtered
  // down to whatever tiles somebody last ticked in the minimap tool.
  render_params.editing_mode = editing_mode::shadow;

  render_params.camera_moved = true;
  render_params.draw_mfbo = false;
  render_params.draw_terrain = true;
  render_params.draw_wmo = clean.include_wmos;

  // Water is a receiver, never a caster: the client does not composite MCSH under liquid and a
  // river surface drawn into the depth buffer would shadow the riverbed beneath it.
  render_params.draw_water = false;

  render_params.draw_wmo_doodads = clean.include_models;
  render_params.draw_models = clean.include_models;

  // Animations off so a bake is reproducible. With them on, the shadow a windmill or a swaying
  // tree casts depends on the value of _world->animtime at the instant the button was pressed,
  // and two bakes of the same tile with the same settings would differ.
  render_params.draw_model_animations = false;

  render_params.draw_models_with_box = false;

  // Hidden models still cast. Hiding a model is an editor convenience for seeing past it, not a
  // statement that it is absent from the world, and a bake that honoured it would bake the
  // editor's view state into the map data.
  render_params.draw_hidden_models = true;

  render_params.draw_sky = false;
  render_params.draw_skybox = false;
  render_params.draw_fog = false;
  render_params.ground_editing_brush = eTerrainType::eTerrainType_Linear;
  render_params.water_layer = 0;
  render_params.display_mode = display_mode::in_3D;
  render_params.draw_occlusion_boxes = false;

  // The one thing this really needs from the minimap path, and the reason it is set even though
  // nothing about this is a minimap. It makes the tile loop at WorldRender.cpp:138-149 accept
  // every loaded tile with no frustum test, force setObjectsFrustumCullTest(2) so M2 and WMO
  // instances skip theirs, and clear the occlusion flag each tile carries from the interactive
  // view. Without it a caster the camera cannot currently see is culled and casts nothing -- and
  // the camera is pointing wherever the user left it, so the bake would depend on the view.
  render_params.minimap_render = true;

  render_params.draw_wmo_exterior = true;
  render_params.render_select_m2_aabb = false;
  render_params.render_select_m2_collission_bbox = false;
  render_params.render_select_wmo_aabb = false;
  render_params.render_select_wmo_groups_bounds = false;
  render_params.draw_db_spawns = false;
  render_params.db_spawns = nullptr;

  glm::vec3 const tile_centre ( tile->xbase + TILESIZE * 0.5f
                              , (extents[0].y + extents[1].y) * 0.5f
                              , tile->zbase + TILESIZE * 0.5f
                              );

  int neighbours_loaded = 0;

  for (MapTile* loaded : _world->mapIndex.loaded_tiles())
  {
    if (loaded && loaded != tile)
    {
      ++neighbours_loaded;
    }
  }

  if (out_neighbour_tiles_loaded)
  {
    *out_neighbour_tiles_loaded = neighbours_loaded;
  }

  // Distance culling is the one cull minimap_render does NOT disable: draw() still skips a tile
  // whose camDist exceeds _cull_distance (WorldRender.cpp:493) and an M2 that fails
  // isInRenderDist (:620). _cull_distance is recomputed from _view_distance inside draw()
  // (:270) with fog off, so raising _view_distance for the duration is what keeps a caster on a
  // neighbouring tile in the scene. camera_pos is the TILE centre rather than the sun's eye,
  // which is what makes that distance mean "how far from the tile being baked".
  float const saved_view_distance = _view_distance;

  // Two tile diagonals past the caster margin, and only ever RAISED. std::max is the whole point
  // of this line: the interactive view distance is a user setting that defaults to 2000 plus a
  // tile radius (WorldRender.cpp:44), which is already larger than the 1708 this computes at the
  // default margin -- so assigning unconditionally would LOWER the cull distance for the bake and
  // drop casters the interactive view was happily drawing. Generous on purpose either way: this
  // is a once-per-tile finishing operation, and over-including costs some wasted rasterisation
  // while under-including costs a silently missing shadow.
  _view_distance = std::max( _view_distance
                           , clean.caster_margin_yards + 2.0f * static_cast<float>(TILE_RADIUS)
                           );

  // The terrain parameter block is snapshotted and put back below, and that is NOT tidiness.
  // draw() overwrites thirteen of its fields whenever minimap_render is set (WorldRender.cpp:96-
  // 110) and NOTHING restores them: draw_shadows in particular is only ever written from the
  // View menu's own toggle (MapView.cpp:3879), which fires on change rather than per frame. Leave
  // it clobbered and the editor stops compositing MCSH the moment a bake finishes -- so the tool
  // whose entire purpose is producing shadows would appear, every single time, to have produced
  // none. The overlay flags (impass, area id, paintability, wireframe, ground effect) are set
  // the same way by their tools and would go out the same door.
  OpenGL::TerrainParamsUniformBlock const saved_terrain_params = _terrain_params_ubo_data;

  bool read_back = false;

  try
  {
    // Single-shot render: WMOGroupRender::upload() must not take its interactive bail-and-retry
    // path, because there is no next frame to retry in and a WMO whose batch textures were still
    // queued would simply be missing from the depth buffer and cast nothing. Same reasoning as
    // saveMinimap, which carries the longer note.
    WMOGroupRender::ScopedBlockingUpload const blocking_upload_guard;

    // View and projection kept SEPARATE rather than collapsed into the projection slot with an
    // identity model-view. The product is all that decides gl_Position, so collapsing them would
    // put the geometry in the right place -- but m2_vert.glsl:96-107 uses model_view on its own
    // to build the view-space position that generates environment-map UVs and camera_dist. Feed
    // it an identity and world coordinates arrive where view coordinates are expected, which
    // changes which texels an alpha-tested doodad discards, and therefore changes the outline of
    // the shadow a tree casts.
    draw( out.light_view
        , out.light_projection
        , glm::vec3()
        , glm::vec4()
        , glm::vec3()
        , tile_centre
        , &depth_pass_settings
        , render_params
        );

    // glFinish and not gl.finish: the OpenGL::context wrapper does not expose finish (it is
    // absent from src/opengl/context.hpp), and saveMinimap reaches for the raw entry point in the
    // same way at WorldRender.cpp:2724. The barrier itself is required -- glReadPixels below is
    // specified to block until the pixels are ready, but the FBO is released immediately
    // afterwards and finishing first keeps that ordering explicit rather than implied.
    glFinish();

    out.depth.resize(static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution));

    // 4-byte rows at every resolution used here, so the default GL_PACK_ALIGNMENT of 4 cannot
    // mis-stride: a row is `resolution` floats and resolution is a multiple of 1024.
    gl.readPixels(0, 0, resolution, resolution, GL_DEPTH_COMPONENT, GL_FLOAT, out.depth.data());

    out.resolution = resolution;
    read_back = true;
  }
  catch (...)
  {
    // The guard above and the FBO below both have to be unwound whatever happens, and this
    // function is reached from a button handler rather than from paintGL -- but an exception
    // escaping toward Qt's event loop past a bound framebuffer would leave the interactive
    // renderer drawing into a dead target for the rest of the session.
    pixel_buffer.release();
    gl.viewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);
    _view_distance = saved_view_distance;
    _terrain_params_ubo_data = saved_terrain_params;
    _need_terrain_params_ubo_update = true;
    throw;
  }

  pixel_buffer.release();
  gl.viewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);
  _view_distance = saved_view_distance;

  // Restored and marked dirty, so the next interactive frame uploads the user's own flags again
  // rather than the ones this pass forced. See the note where the snapshot is taken.
  _terrain_params_ubo_data = saved_terrain_params;
  _need_terrain_params_ubo_update = true;

  if (!read_back || !out.valid())
  {
    out = Noggit::Rendering::SunDepthMap();
    return false;
  }

  return true;
}

bool WorldRender::saveMinimap(TileIndex const& tile_idx, MinimapRenderSettings* settings, std::optional<QImage>& combined_image)
{
  ZoneScoped;
  // Setup framebuffer
  QOpenGLFramebufferObjectFormat fmt;
  fmt.setSamples(0);
  fmt.setInternalTextureFormat(GL_RGBA8);
  fmt.setAttachment(QOpenGLFramebufferObject::Depth);

  QOpenGLFramebufferObject pixel_buffer(settings->resolution, settings->resolution, fmt);
  pixel_buffer.bind();

  gl.viewport(0, 0, settings->resolution, settings->resolution);
  gl.clearColor(.0f, .0f, .0f, 1.f);
  gl.clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  // Load tile
  bool unload = !_world->mapIndex.has_unsaved_changes(tile_idx);

  if (!_world->mapIndex.tileLoaded(tile_idx) && !_world->mapIndex.tileAwaitingLoading(tile_idx))
  {
    MapTile* tile = _world->mapIndex.loadTile(tile_idx);
    tile->wait_until_loaded();
    _world->wait_for_all_tile_updates();
    tile->waitForChildrenLoaded();
  }

  MapTile* mTile = _world->mapIndex.getTile(tile_idx);

  if (mTile)
  {
    unsigned counter = 0;
    constexpr unsigned TIMEOUT = 5000;

    while (AsyncLoader::instance->is_loading() || !mTile->finishedLoading())
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      counter++;

      if (counter >= TIMEOUT)
        break;
    }

    float max_height = std::max(_world->getMaxTileHeight(tile_idx), 200.f);

    // setup view matrices
    auto projection = glm::ortho( -TILESIZE / 2.0f,TILESIZE / 2.0f,-TILESIZE / 2.0f,TILESIZE / 2.0f,0.f,100000.0f);

    auto eye = glm::vec3(TILESIZE * tile_idx.x + TILESIZE / 2.0f, max_height + 10.0f, TILESIZE * tile_idx.z + TILESIZE / 2.0f);
    auto center = glm::vec3(TILESIZE * tile_idx.x + TILESIZE / 2.0f, max_height + 5.0f, TILESIZE * tile_idx.z + TILESIZE / 2.0 - 0.005f);
    auto up = glm::vec3(0.f, 1.f, 0.f);

    glm::vec3 const z = glm::normalize(eye - center);
    glm::vec3 const x = glm::normalize(glm::cross(up, z));
    glm::vec3 const y = glm::normalize(glm::cross(z, x));

    auto look_at = glm::transpose(glm::mat4x4(x.x, x.y, x.z, glm::dot(x, glm::vec3(-eye.x, -eye.y, -eye.z))
        , y.x, y.y, y.z, glm::dot(y, glm::vec3(-eye.x, -eye.y, -eye.z))
        , z.x, z.y, z.z, glm::dot(z, glm::vec3(-eye.x, -eye.y, -eye.z))
        , 0.f, 0.f, 0.f, 1.f
    ));

    glFinish();

    // This is a single-shot render whose output is written to a file, so WMOGroupRender::upload()
    // must not use its interactive bail-and-retry path here: there is no next frame to retry in,
    // and a WMO whose batch textures were still queued would be left out of the written image
    // with no warning. The spin above is not enough on its own -- AsyncLoader::is_loading() only
    // reports objects a worker has already picked up (AsyncLoader.cpp:23-27, against the separate
    // _to_load queues), so it reads false while a backlog is still waiting, and the
    // waitForChildrenLoaded() above it is skipped entirely for a tile that was already resident.
    //
    // Blocking is confined to this scope, which is what the guard is for: the interactive
    // renderer keeps bailing and retrying, which is the behaviour worth having.
    WMOGroupRender::ScopedBlockingUpload const blocking_upload_guard;

    drawMinimap(mTile
        , look_at
        , projection
        , glm::vec3(TILESIZE * tile_idx.x + TILESIZE / 2.0f
            , max_height + 15.0f, TILESIZE * tile_idx.z + TILESIZE / 2.0f)
        , settings);

    // Clearing alpha from image
    gl.colorMask(false, false, false, true);
    gl.clearColor(0.0f, 0.0f, 0.0f, 1.0f);
    gl.clear(GL_COLOR_BUFFER_BIT);
    gl.colorMask(true, true, true, true);

    assert(pixel_buffer.isValid() && pixel_buffer.isBound());

    QImage image = pixel_buffer.toImage();

    image = image.convertToFormat(QImage::Format_RGBA8888);

    QString str = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
    if (!(str.endsWith('\\') || str.endsWith('/')))
    {
      str += "/";
    }

    QString target_dir = QString("/textures/minimap/");
    if(settings->export_mode == MinimapGenMode::LOD_MAPTEXTURES || settings->export_mode == MinimapGenMode::LOD_MAPTEXTURES_N)
	{
	  target_dir = QString("/textures/maptextures/");
	}

    QDir dir(str + target_dir);
    if (!dir.exists())
      dir.mkpath(".");

    std::string tex_name = std::string(_world->basename + "_" + std::to_string(tile_idx.x) + "_" + std::to_string(tile_idx.z) + ".blp");
    if (settings->export_mode == MinimapGenMode::LOD_MAPTEXTURES_N)
    {
        tex_name = std::string(_world->basename + "_" + std::to_string(tile_idx.x) + "_" + std::to_string(tile_idx.z) + "_n.blp");
    }

    if (settings->file_format == ".png")
    {
      image.save(dir.filePath(std::string(_world->basename + "_" + std::to_string(tile_idx.x) + "_" + std::to_string(tile_idx.z) + ".png").c_str()));
    }
    else if (settings->file_format == ".blp (DXT1)" || settings->file_format == ".blp (DXT5)")
    {
      QByteArray bytes;
      QBuffer buffer( &bytes );
      buffer.open( QIODevice::WriteOnly );

      image.save( &buffer, "PNG" );

      auto blp = Png2Blp();
      blp.load(reinterpret_cast<const void*>(bytes.constData()), bytes.size());

      uint32_t file_size;
      // void* blp_image = blp.createBlpDxtInMemory(true, FORMAT_DXT5, file_size);
      // this mirrors blizzards : dxt1, no mipmap
      void* blp_image = blp.createBlpDxtInMemory(settings->file_format == ".blp (DXT5)" ? true : false, settings->file_format == ".blp (DXT5)" ? FORMAT_DXT5 : FORMAT_DXT1, file_size);

      // converts the texture name to an md5 hash like blizzard, this is used to avoid duplicates textures for ocean
      // downside is that if the file gets updated regularly there will be a lot of duplicates in the project folder
      // probably should be a patching option when deploying
      bool use_md5 = false;
      if (use_md5)
      {
          QCryptographicHash md5_hash(QCryptographicHash::Md5);
          // auto data = reinterpret_cast<char*>(blp_image);
          md5_hash.addData(reinterpret_cast<char*>(blp_image), file_size);
          auto resulthex = md5_hash.result().toHex().toStdString() + ".blp";
          tex_name = resulthex;
      }


      QFile file(dir.filePath(tex_name.c_str()));
      file.open(QIODevice::WriteOnly);

      QDataStream out(&file);
      out.writeRawData(reinterpret_cast<char*>(blp_image), file_size);

      file.close();
    }

    // Write combined file
    if (settings->combined_minimap && combined_image.has_value())
    {
      QImage scaled_image = image.scaled(128, 128,  Qt::KeepAspectRatio);

      for (int i = 0; i < 128; ++i)
      {
        for (int j = 0; j < 128; ++j)
        {
          combined_image->setPixelColor(static_cast<int>(tile_idx.x) * 128 + j, static_cast<int>(tile_idx.z) * 128 + i, scaled_image.pixelColor(j, i));
        }
      }

    }

    // Register in md5translate.trs
    try
    {
        std::string map_name = gMapDB.getByID(_world->mapIndex._map_id).getString(MapDB::InternalName);
        auto sstream = std::stringstream();
        sstream << map_name << "\\map" << tile_idx.x << "_" << std::setfill('0') << std::setw(2) << tile_idx.z << ".blp";
        std::string tilename_left = sstream.str();
        auto& minimap_md5translate = Noggit::Application::NoggitApplication::instance()->clientData()->_minimap_md5translate;
        minimap_md5translate[map_name][tilename_left] = tex_name;
    }
    catch(MapDB::NotFound)
    {
        LogError << "SaveMinimap : Couldn't find entry " << _world->mapIndex._map_id << std::endl;
        assert(false);
    }

    if (unload)
    {
      _world->mapIndex.unloadTile(tile_idx);
    }

  }

  pixel_buffer.release();

  return true;
}

[[nodiscard]]
OpenGL::TerrainParamsUniformBlock* Noggit::Rendering::WorldRender::getTerrainParamsUniformBlock()
{
  return &_terrain_params_ubo_data;
}
