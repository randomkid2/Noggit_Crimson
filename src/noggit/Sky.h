// This file is part of Noggit3, licensed under GNU General Public License (version 3).
#pragma once
#include <noggit/DBCFile.h>
#include <noggit/ModelInstance.h>
#include <noggit/ContextObject.hpp>
#include <noggit/rendering/Primitives.hpp>
#include <opengl/scoped.hpp>

#include <memory>
#include <string>
#include <vector>

constexpr int DAY_DURATION = 2880; // Time Values from 0 to 2880 where each number represents a half minute from midnight to midnight

// 3.3.5 only
enum SkyColorNames
{
  LIGHT_GLOBAL_DIFFUSE,
  LIGHT_GLOBAL_AMBIENT,
  SKY_COLOR_TOP, // top
  SKY_COLOR_MIDDLE, // middle
  SKY_COLOR_BAND1, // middle to horizon
  SKY_COLOR_BAND2, // above horizon
  SKY_COLOR_SMOG, // horizon/smog
  SKY_FOG_COLOR, // fog and WDL mountains
  SHADOW_OPACITY, // Unknown / unused in 3.3.5 ? This value was moved to ShadowOpacity(17) in the new format
  SUN_COLOR, // sun, specular light, sunrays
  SUN_CLOUD_COLOR, // bigger sun halo
  CLOUD_EMISSIVE_COLOR, // cloud edge
  CLOUD_LAYER1_AMBIENT_COLOR, // cloud body
  CLOUD_LAYER2_AMBIENT_COLOR, // Unknown / unused in 3.3.5 ? This value was ported to Cloud Layer 2 Ambient Color in the new format
  OCEAN_COLOR_LIGHT, // shallow ocean
  OCEAN_COLOR_DARK, // deep ocean
  RIVER_COLOR_LIGHT, // shallow river
  RIVER_COLOR_DARK, // deep river
  NUM_SkyColorNames
};

enum SkyFloatParamsNames
{
  SKY_FOG_DISTANCE,
  SKY_FOG_MULTIPLIER,
  SKY_CELESTIAL_GLOW,
  SKY_CLOUD_DENSITY,
  SKY_UNK_FLOAT_PARAM_4,
  SKY_UNK_FLOAT_PARAM_5,
  NUM_SkyFloatParamsNames
};

enum SkyParamsNames
{
  SKY_PARAM_CLEAR,
  SKY_PARAM_CLEAR_UNDERWATER,
  SKY_PARAM_TORM,
  SKY_PARAM_STORM_UNDERWATER,
  SKY_PARAM_DEATH,
  SKY_PARAM_UNK_1,
  SKY_PARAM_UNK_2,
  SKY_PARAM_UNK_3,
  NUM_SkyParamsNames
};

enum SkyModelSkyBoxFlags
{
  LIGHT_SKYBOX_FULL_DAY = 0x1, // Full day Skybox: animation syncs with time of day (uses animation 0, time of day is just in percentage).
  LIGHT_SKYBOX_COMBINE = 0x2, // Combine Procedural And Skybox : render stars, sun and moons and clouds as well
  /*  
  0x04	Procedural Fog Color Blend
  0x08	Force Sun-shafts
  0x10	Disable use Sun Fog Color
  */
};

struct OutdoorLightStats
{
  float nightIntensity;
  glm::vec3 dayDir;

  void interpolate(OutdoorLightStats *a, OutdoorLightStats *b, float r);
};

class OutdoorLighting
{
private:
  std::vector<OutdoorLightStats> lightStats;

public:
  OutdoorLighting();

  OutdoorLightStats getLightStats(int time);
};

struct ZoneLight
{
  unsigned int id = 0;
  std::string name;
  // unsigned int mapId = 0; // map.dbc
  unsigned int lightId = 0; // light.dbc reference

  std::vector< glm::vec2> points;

  // added in 8.2
  // float zMin = -64000.0f;
  // float zMin = 64000.0f;

  std::array<glm::vec2, 2> _extents; // 2d square corners for fast bounds check before precise polygon intersection check
  // math::aabb_2d extents;

  // Sky* light = nullptr; 
  // std::vector<ZoneLightPoint> points;
};

struct ZoneLightPoint
{
    unsigned int id = 0;
    unsigned int zoneLightId = 0;
    float posX = 0.0f;
    float posY = 0.0f;
    unsigned int pointOrder;
};

struct SkyColor 
{
  glm::vec3 color;
  int time;

  SkyColor(int t, int col);
};

struct SkyFloatParam
{
  SkyFloatParam(int t, float val);

  float value;
  int time;
};

// TODO modern LightData.db
// unified timestamps for float and int data
// old data should fit in it as it does in classic
/*
struct LightData
{
  LightData(int paramId);
  
  unsigned int paramId;
  unsigned int time = 0;
  glm::vec3 colorRows[NUM_SkyColorNames] = {};
  float floatParams[NUM_SkyParamsNames] = {};
};*/

class SkyParam
{
public:
    std::optional<ModelInstance> skybox;
    int skyboxFlags = 0;

    int Id;

    SkyParam() = default;
    explicit SkyParam(int paramId, Noggit::NoggitRenderContext context);

    //array of 18 vectors(for each color), each vector item is a time/value. There can only be up to 16 vector items
    std::vector<SkyColor> colorRows[NUM_SkyColorNames];
    std::vector<SkyFloatParam> floatParams[NUM_SkyFloatParamsNames];

    // potential structure rework, more similar to retail/classic LightData.db
    // std::vector<LightData> lightData;

    bool highlight_sky() const;
    float river_shallow_alpha() const;
    float river_deep_alpha() const;
    float ocean_shallow_alpha() const;
    float ocean_deep_alpha() const;
    float glow() const;

    void set_glow(float glow);
    void set_highlight_sky(bool state);
    void set_river_shallow_alpha(float alpha);
    void set_river_deep_alpha(float alpha);
    void set_ocean_shallow_alpha(float alpha);
    void set_ocean_deep_alpha(float alpha);

    // always save them for now
    // later we can have a system to only save modified dbcs
    bool _need_save = true;
    bool _colors_need_save = true;
    bool _floats_need_save = true;
    bool _is_new_param_record = false;

private: 
    // most common settings
    bool _highlight_sky = false;
    float _river_shallow_alpha = 0.5f;
    float _river_deep_alpha = 1.0f;
    float _ocean_shallow_alpha = 0.75f;
    float _ocean_deep_alpha = 1.0f;
    float _glow = 0.5f;
    // int _cloud_type = 0; // always 0 in 3.3.5

    Noggit::NoggitRenderContext _context;
};

namespace Noggit
{
  // A light lifted out of Light.dbc, complete enough to recreate it in any map.
  //
  // A value type and not a Sky* on purpose: the clipboard has to outlive the Skies object of the
  // map it was copied from, because copying a light in one map and pasting it into another is the
  // entire point of the cross-map browser. Skies is destroyed and rebuilt on every map load.
  struct LightSnapshot
  {
    bool valid = false;

    // Where it came from. Kept for the paste log and the browser label only -- neither is written
    // to any DBC, because a paste always allocates a fresh Light.dbc id and stamps the map the
    // editor currently has open.
    int light_id = 0;
    int map_id = -1;
    std::string name;

    glm::vec3 pos = glm::vec3(0.f, 0.f, 0.f);
    float r1 = 0.f;
    float r2 = 0.f;

    // LightParams.dbc ids, in Light.dbc DataIDs order. Zero means "this weather slot is unused",
    // which is normal: of the eight slots only the first five are ever populated in 3.3.5 data.
    unsigned int param_ids[NUM_SkyParamsNames] = {};
  };

  // Process-wide light clipboard, one entry, replaced by each copy. Lives in Sky.cpp.
  LightSnapshot const& lightClipboard();
  void setLightClipboard(LightSnapshot const& snapshot);

  // Reads one Light.dbc row into a snapshot. Works for ANY map, including maps not loaded, which
  // is what makes the cross-map browser possible at all -- Skies only ever holds the loaded map.
  //
  // Declared here and defined in Sky.cpp because the 36.0 factor between Light.dbc coordinates and
  // Noggit world coordinates lives there and nowhere else. A second copy of that constant in the
  // UI is precisely how a pasted light ends up thirty-six times too far from the origin.
  bool lightSnapshotFromDbc(int light_id, LightSnapshot& out);
}

class Sky 
{
private:
  mutable SkyParam* cachedCurrentParam = nullptr;

public:
  // std::optional<ModelInstance> skybox;
  int Id;
  glm::vec3 pos = glm::vec3(0, 0, 0);
  float r1 = 0.f, r2 = 0.f;
  std::string name;

  explicit Sky(DBCFile::Iterator data, Noggit::NoggitRenderContext context);

  // Builds a light from a clipboard snapshot instead of a DBC row, for paste and duplicate.
  //
  // `map_id` is the map the light is being pasted INTO, never the one it came from -- that is the
  // whole reason the snapshot carries its source map only as a label.
  Sky( Noggit::LightSnapshot const& snapshot
     , int new_id
     , int map_id
     , Noggit::NoggitRenderContext context
     );

  int getId() const;;

  // std::unique_ptr<SkyParam> skyParams[NUM_SkyParamsNames];
  unsigned int skyParams[NUM_SkyParamsNames];
  int curr_sky_param = SKY_PARAM_CLEAR;


  std::optional<SkyParam*> getParam(int param_index) const;
  std::optional<SkyParam*> getCurrentParam() const;;


  glm::vec3 colorFor(int r, int t) const;
  float floatParamFor(int r, int t) const;

  float weight = 0.0f;
  bool global = false;
  bool zone_light = false;

  bool is_new_record = false;

  bool operator<(const Sky& s) const;

  bool selected() const;

  // The selection flag was declared, initialised false and read by selected(), and no line in the
  // tree ever wrote it -- the plumbing was started and abandoned. Skies::setSelectedLight is the
  // only caller, so exactly one light can be flagged at a time.
  void setSelected(bool selected);

  // Light.dbc column 1. It was private with no setter and no friend, which is what made a
  // cross-map copy impossible rather than merely awkward: the old duplicate path copied a Sky by
  // value, so a light duplicated out of another map inherited that map's id, and save_to_dbc
  // wrote exactly that id back (Sky.cpp, LightDB::Map). Pasting Stormwind's light into a custom
  // map produced a Light.dbc row pointing at map 0, i.e. a second light for Azeroth.
  [[nodiscard]] int mapId() const;
  void setMapId(int map_id);

  // True for the borrowed Azeroth global light that Skies substitutes when the loaded map has no
  // global light of its own (see Skies::Skies). It is Light.dbc id 1, it belongs to map 0, and
  // saving it from another map's editor would rewrite Azeroth's global lighting for every zone on
  // that continent. validateForSave refuses it by name.
  bool is_fallback_global = false;

  // Would save_to_dbc produce a Light.dbc set the client can read back? Writes nothing.
  //
  // Split out so the panel can grey a button out and show the reason before the user commits, and
  // so save_to_dbc can run the whole check before it touches a single record. A light spans five
  // linked DBCs and a half-written set is a corrupt lighting table for the entire map, so the only
  // safe order is: decide everything first, then write. `reason` is filled in only on false.
  [[nodiscard]] bool validateForSave(std::string& reason) const;

  // Writes this light and every DBC row it owns. False means NOTHING was written at all.
  //
  // Note the return value cannot promise the bytes reached the disk: DBCFile::save() is void and
  // non-throwing by design (it logs and returns, leaving the previous file intact). What false
  // does promise is that validation refused before any record was touched.
  bool save_to_dbc();

private:
  bool _selected;

  int _map_id = 0; // Light.dbc column 1, only ever read back out by save_to_dbc

  Noggit::NoggitRenderContext _context;
};

class Skies 
{
private:
  void loadZoneLights(int map_id);

  int _map_id = 0;
  int _selected_light_id = 0;

  int numSkies = 0;
  int cs = -1;
  ModelInstance stars;

  bool _force_update = true;
  int _last_time = -1;
  glm::vec3 _last_pos;

  // active render settings after blending between active lights
  // Look at Sky for individual light settings

  float _river_shallow_alpha = 0.5f;
  float _river_deep_alpha = 1.0f;
  float _ocean_shallow_alpha = 0.75f;
  float _ocean_deep_alpha = 1.0f;
  float _glow = 0.5f;
  // bool _highlight_sky = false; // since it's a bool and it can't be blended, just get from highest prio sky

  float _fog_rate = 1.5f;

  // float params
  float _fog_distance = 6500.0f;
  float _fog_multiplier = 0.1f;
  float _celestial_glow = 1.0f;
  float _cloud_density = 1.0f;
  float _unknown_float_param4 = 1.0f;
  float _unknown_float_param5 = 1.0f;

public:
  // Light Zones
  // hardcoded in the client in 3.3.5, they were moved to a DBC in 4.0
  std::vector<ZoneLight> zoneLightsWotlk;
  // std::unordered_map<int, std::vector<ZoneLightPoint>> zoneLightPoints; // grouped by zoneLightId. <zoneLightId, points>
  // std::vector<ZoneLightPoint> zoneLightPoints = {
  //     {300, 60, 4215.8745f, 3269.2654f, 1},
  // };
  bool using_fallback_global = false; // if map doesn't have a global

  SkyParamsNames active_param;

  std::vector<Sky> skies;
  std::array<glm::vec3, NUM_SkyColorNames> color_set = { glm::vec3(1.f, 1.f, 1.f) };
  Sky* findSkyById(int sky_id);

  explicit Skies(unsigned int mapid, Noggit::NoggitRenderContext context);

  // The map these lights were loaded for. Needed by every paste: the light being pasted carries
  // the map it came from, and the row that gets written has to carry this one instead.
  [[nodiscard]] int mapId() const;

  // The light the viewport gizmo and the editor panel are both pointed at, by Light.dbc id.
  // Zero means nothing is selected.
  //
  // An id and never a Sky*: findSkyWeights re-sorts `skies` by distance on every call, and
  // pasteLight push_backs into it, so any Sky* held across a frame either names a different light
  // or points at freed memory. Every consumer resolves through findSkyById instead.
  void setSelectedLight(int light_id);
  [[nodiscard]] int selectedLight() const;

  // Pastes a clipboard snapshot into THIS map under a freshly allocated Light.dbc id.
  //
  // This replaced createNewSky, which took a Sky* and called save_to_dbc() on the spot -- so
  // pressing Duplicate rewrote Light.dbc, LightParams.dbc and LightIntBand.dbc in the project
  // directory with no confirmation and no way back. Duplicate now goes through the same clipboard
  // this does, so there is one creation path rather than two, and the new light is an unsaved
  // in-memory record (is_new_record stays true) with the explicit Save button owning the write.
  //
  // `deep_copy_params` decides what the pasted light's colours are attached to. False reuses the
  // source's LightParams ids, which is one row of Light.dbc and nothing else -- but editing your
  // zone's colours then edits every other light that shares the param, including Blizzard's.
  // True allocates new LightParams / LightIntBand / LightFloatBand rows so the copy is
  // independent, at the cost of 1 + 18 + 6 new rows per populated weather slot.
  //
  // A snapshot whose position is 0,0,0 creates the map's GLOBAL light, and retires the borrowed
  // one this map may have been rendering with. That is not a special case bolted on: a light at the
  // origin is what a global light is, here and in the client.
  //
  // Returns nullptr with `error` filled in. Nothing is written here either; Save owns that.
  Sky* pasteLight(Noggit::LightSnapshot const& snapshot, bool deep_copy_params, std::string& error);

  // Removes a light from this map. Only Light.dbc is touched, and only when the row exists there.
  //
  // LightParams / LightIntBand / LightFloatBand rows are deliberately left behind: params are
  // shared between lights -- the editor panel already counts the users of one -- and cascading a
  // delete through them would silently recolour unrelated zones. An orphaned param row is
  // unreachable from Light.dbc and therefore invisible to the client.
  bool deleteSky(int light_id, std::string& error);

  // Captures a loaded light as a snapshot WITHOUT touching the clipboard. False when not loaded.
  //
  // Separate from copyLightToClipboard because Duplicate and the browser's paste both need a
  // snapshot and neither should silently replace whatever the user copied ten minutes ago.
  bool snapshotLight(int light_id, std::string const& name, Noggit::LightSnapshot& out) const;

  // Copies a light out of THIS map into the clipboard. False when the id is not loaded.
  bool copyLightToClipboard(int light_id, std::string const& name);

  Sky* findSkyWeights(glm::vec3 pos);

  Sky* findClosestSkyByWeight();
  Sky* findClosestSkyByDistance(glm::vec3 pos);

  void setCurrentParam(int param_id);
  void update_sky_colors(glm::vec3 pos, int time, bool global_only);

  bool draw ( glm::mat4x4 const& model_view
            , glm::mat4x4 const& projection
            , glm::vec3 const& camera_pos
            , OpenGL::Scoped::use_program& m2_shader
            , math::frustum const& frustum
            , const float& cull_distance
            , int animtime
            , int time
            /*, bool draw_particles*/
            , bool draw_skybox
            , OutdoorLightStats const& light_stats
            );

  // drawLightingSpheres and drawLightingSphereHandles used to be declared here. Both were dead --
  // defined in Sky.cpp and called from nowhere in the tree -- and both were traps for anyone who
  // revived them: the first ran an unconditional Log << inside its per-sky loop, i.e. one log line
  // per light per frame, and the second passed a 0.3 alpha in the colour vec4 that
  // Primitives::Sphere::draw discards in favour of its separate `alpha` parameter (which defaults
  // to 1.f), so it would have drawn opaque spheres. WorldRender::draw already renders the light
  // spheres properly, with the inside/outside cull split and the toolbar's alpha and wireframe.

  bool hasSkies() const;

  float river_shallow_alpha() const;
  float river_deep_alpha() const;
  float ocean_shallow_alpha() const;
  float ocean_deep_alpha() const;

  float fog_distance_end() const;;
  float fog_distance_start() const;;
  float fog_distance_multiplier() const;;

  float celestial_glow() const;;
  float cloud_density() const;;
  float unknown_float_param4() const;;
  float unknown_float_param5() const;;

  float glow() const;;

  float fogRate() const;

  void unload();

  void force_update();

private:
  bool _uploaded = false;
  bool _need_color_buffer_update = true;
  bool _need_vao_update = true;

  int _indices_count;

  void upload();
  void update_color_buffer();
  void update_vao(OpenGL::Scoped::use_program& shader);

  OpenGL::Scoped::deferred_upload_vertex_arrays<1> _vertex_array;
  GLuint const& _vao = _vertex_array[0];
  OpenGL::Scoped::deferred_upload_buffers<3> _buffers;
  GLuint const& _vertices_vbo = _buffers[0];
  GLuint const& _colors_vbo = _buffers[1];
  GLuint const& _indices_vbo = _buffers[2];

  std::unique_ptr<OpenGL::program> _program;

  Noggit::NoggitRenderContext _context;

  Noggit::Rendering::Primitives::Sphere _sphere_render;
};
