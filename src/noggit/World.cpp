// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <math/trig.hpp>
#include <noggit/ActionManager.hpp>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/Brush.h> // brush
#include <noggit/ChunkWater.hpp>
#include <noggit/Log.h>
#include <noggit/MapChunk.h>
#include <noggit/MapTile.h>
#include <noggit/Misc.h>
#include <noggit/Model.h>
#include <noggit/ModelInstance.h>
#include <noggit/ModelManager.h> // ModelManager
#include <noggit/object_paste_params.hpp>
#include <noggit/project/CurrentProject.hpp>
#include <noggit/texture_set.hpp>
#include <noggit/TextureManager.h>
#include <noggit/TileIndex.hpp>
#include <noggit/tool_enums.hpp>
#include <noggit/ui/TexturingGUI.h>
#include <noggit/WMOInstance.h> // WMOInstance
#include <noggit/World.h>
#include <noggit/World.inl>

#include <math/bounding_box.hpp>

#include <blizzard-database-library/include/structures/FileStructures.h>

#include <external/tracy/Tracy.hpp>

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QMessageBox>
#include <QProgressDialog>
#include <QSettings>

#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>


bool World::IsEditableWorld(BlizzardDatabaseLib::Structures::BlizzardDatabaseRow& record)
{
  ZoneScoped;
  std::string lMapName = record.Columns["Directory"].Value;

  std::stringstream ssfilename;
  ssfilename << "World\\Maps\\" << lMapName << "\\" << lMapName << ".wdt";

  if (!Noggit::Application::NoggitApplication::instance()->clientData()->exists(ssfilename.str()))
  {
    LogDebug << "World " << record.RecordId << ": " << lMapName << " has no WDT file!" << std::endl;
    return false;
  }

  BlizzardArchive::ClientFile mf(ssfilename.str(), Noggit::Application::NoggitApplication::instance()->clientData());

  //sometimes, wdts don't open, so ignore them...
  if (mf.isEof())
    return false;

  const char * lPointer = reinterpret_cast<const char*>(mf.getPointer());

  // Not using the libWDT here doubles performance. You might want to look at your lib again and improve it.
  const int lFlags = *(reinterpret_cast<const int*>(lPointer + 8 + 4 + 8));

  // check for global wmo flag
  if (lFlags & FLAG_GLOBAL_OBJECT)
    return true; // filter them later

  // check if map has tiles
  const int * lData = reinterpret_cast<const int*>(lPointer + 8 + 4 + 8 + 0x20 + 8);
  for (int i = 0; i < 8192; i += 2)
  {
    if (lData[i] & 1)
      return true;
  }

  // change : still load world even if it has no tile to allow user to edit it
  return true;
}

bool World::IsWMOWorld(BlizzardDatabaseLib::Structures::BlizzardDatabaseRow& record)
{
    ZoneScoped;
    std::string lMapName = record.Columns["Directory"].Value;

    std::stringstream ssfilename;
    ssfilename << "World\\Maps\\" << lMapName << "\\" << lMapName << ".wdt";

    BlizzardArchive::ClientFile mf(ssfilename.str(), Noggit::Application::NoggitApplication::instance()->clientData());

    const char* lPointer = reinterpret_cast<const char*>(mf.getPointer());

    const int lFlags = *(reinterpret_cast<const int*>(lPointer + 8 + 4 + 8));
    if (lFlags & 1)
        return true;

    return false;
}

World::World(const std::string& name, int map_id, Noggit::NoggitRenderContext context, bool create_empty)
    : _renderer(Noggit::Rendering::WorldRender(this))
    , _model_instance_storage(this)
    , _tile_update_queue(this)
    , mapIndex(name, map_id, this, context, create_empty)
    , horizon(name, &mapIndex)
    , mWmoFilename(mapIndex.globalWMOName)
    , mWmoEntry(mapIndex.wmoEntry)
    , animtime(0)
    , time(1450)
    , basename(name)
    , _current_selection()
    , _settings(new QSettings())
    , _context(context)
{
  LogDebug << "Loading world \"" << name << "\"." << std::endl;
  _loaded_tiles_buffer[0] = std::make_pair<std::pair<int, int>, MapTile*>(std::make_pair(0, 0), nullptr);
}

void World::LoadSavedSelectionGroups()
{
  _selection_groups.clear();

  auto& saved_map_groups = Noggit::Project::CurrentProject::get()->ObjectSelectionGroups;
  for (auto& map_group : saved_map_groups)
  {
      if (map_group.MapId == mapIndex._map_id)
      {
          for (auto& group : map_group.SelectionGroups)
          {
              selection_group selectionGroup(group, this);
              _selection_groups.push_back(selectionGroup);
          }
          return;
      }
  }
}

void World::saveSelectionGroups()
{
    auto proj_selection_map_group = Noggit::Project::NoggitProjectSelectionGroups();
    proj_selection_map_group.MapId = mapIndex._map_id;
    for (auto& selection_group : _selection_groups)
    {
        proj_selection_map_group.SelectionGroups.push_back(selection_group.getMembers());
    }

    Noggit::Project::CurrentProject::get()->saveObjectSelectionGroups(proj_selection_map_group);
}

Noggit::Rendering::WorldRender* World::renderer()
{
  return &_renderer;
}

void World::update_selection_pivot()
{
  ZoneScoped;
  if (has_multiple_model_selected())
  {
    glm::vec3 pivot = glm::vec3(0);
    int model_count = 0;

    for (auto const& entry : _current_selection)
    {
      if (entry.index() == eEntry_Object)
      {
        pivot += std::get<selected_object_type>(entry)->pos;
        model_count++;
      }
    }

    _multi_select_pivot = pivot / static_cast<float>(model_count);
  }
  else
  {
    _multi_select_pivot = std::nullopt;
  }
}

std::optional<glm::vec3> const& World::multi_select_pivot() const
{
  return _multi_select_pivot;
}

bool World::is_selected(selection_type selection)
{
  ZoneScoped;
  if (selection.index() != eEntry_Object)
    return false;

  /*
  auto which = std::get<selected_object_type>(selection)->which();

  if (which == eMODEL)
  {
    uint uid = static_cast<ModelInstance*>(std::get<selected_object_type>(selection))->uid;
    auto const& it = std::find_if(_current_selection.begin()
                                  , _current_selection.end()
                                  , [uid] (selection_type type)
    {
      return var_type(type) == typeid(selected_object_type)
        && std::get<selected_object_type>(type)->which() == eMODEL
        && static_cast<ModelInstance*>(std::get<selected_object_type>(type))->uid == uid;
    }
    );

    if (it != _current_selection.end())
    {
      return true;
    }
  }
  else if (which == eWMO)
  {
    uint uid = static_cast<WMOInstance*>(std::get<selected_object_type>(selection))->uid;
    auto const& it = std::find_if(_current_selection.begin()
                            , _current_selection.end()
                            , [uid] (selection_type type)
    {
      return var_type(type) == typeid(selected_object_type)
        && std::get<selected_object_type>(type)->which() == eWMO
        && static_cast<WMOInstance*>(std::get<selected_object_type>(type))->uid == uid;
    }
    );
    if (it != _current_selection.end())
    {
      return true;
    }
  }


  return false;
*/

  auto selected_object = std::get<selected_object_type>(selection);
  unsigned int uid = selected_object->uid;

  bool found = selected_uids.contains(uid);
  if (!found)
    return false;

  // verify object type
  // probably should only be done when adding or removing objects.
  /*
  auto instance = getObjectInstance(uid);
  if (instance == nullptr || var_type(instance) != typeid(selected_object_type))
    return false;

  if (selected_object->which() != instance->which())
  {
    return false;
  }
  */
  return true;
}

bool World::is_selected(std::uint32_t uid) const
{
  return selected_uids.contains(uid);
}

std::vector<selection_type> const& World::current_selection() const
{
  return _current_selection;
}

std::optional<selection_type> World::get_last_selected_model() const
{
  ZoneScoped;
  if (_current_selection.empty())
      return std::nullopt;

  auto const it
    ( std::find_if ( _current_selection.rbegin()
                   , _current_selection.rend()
                   , [&] (selection_type const& entry)
                     {
                       return entry.index() != eEntry_MapChunk;
                     }
                   )
    );

  return it == _current_selection.rend()
    ? std::optional<selection_type>() : std::optional<selection_type> (*it);
}

bool World::has_selection() const
{
  return !_current_selection.empty();
}

bool World::has_multiple_model_selected() const
{
  return _selected_model_count > 1;
}

int World::get_selected_model_count() const
{
  return _selected_model_count;
}

std::vector<selected_object_type> const World::get_selected_objects() const
{
    // std::vector<selected_object_type> objects(_selected_model_count);
    std::vector<selected_object_type> objects;
    objects.reserve(_selected_model_count);

    ZoneScoped;
    for (auto& entry : _current_selection)
    {
        if (entry.index() == eEntry_Object)
        {
            auto obj = std::get<selected_object_type>(entry);
            objects.push_back(obj);
        }
    }

    return objects;
}

glm::vec3 getBarycentricCoordinatesAt(
    const glm::vec3& a,
    const glm::vec3& b,
    const glm::vec3& c,
    const glm::vec3& point,
    const glm::vec3& normal)
{
  glm::vec3 bary;
  // The area of a triangle is

  glm::vec3 aMb = (b - a);
  glm::vec3 cMa = (c - a);
  glm::vec3 bMpoint = (b - point);
  glm::vec3 cMpont = (c - point);
  glm::vec3 aMpoint = (a - point);

  glm::vec3 ABC = glm::cross(aMb ,cMa);
  glm::vec3 PBC = glm::cross(bMpoint, cMpont);
  glm::vec3 PCA = glm::cross(cMpont, aMpoint);

  double areaABC = glm::dot(normal , ABC);
  double areaPBC = glm::dot(normal , PBC);
  double areaPCA = glm::dot(normal , PCA);

  bary.x = areaPBC / areaABC; // alpha
  bary.y = areaPCA / areaABC; // beta
  bary.z = 1.0f - bary.x - bary.y; // gamma

  return bary;
}

void World::rotate_selected_models_randomly(float minX, float maxX, float minY, float maxY, float minZ, float maxZ)
{
  ZoneScoped;
  bool has_multi_select = has_multiple_model_selected();

  for (auto& entry : _current_selection)
  {
    auto type = entry.index();
    if (type == eEntry_MapChunk)
    {
      continue;
    }

    updateTilesEntry(entry, model_update::remove);

    auto& obj = std::get<selected_object_type>(entry);
    NOGGIT_CUR_ACTION->registerObjectTransformed(obj);

    math::degrees::vec3& dir = obj->dir;

    float rx = misc::randfloat(minX, maxX);
    float ry = misc::randfloat(minY, maxY);
    float rz = misc::randfloat(minZ, maxZ);

    //Building rotations
    auto heading = math::radians(math::degrees(dir.z))._ * 0.5;
    auto attitude = math::radians(math::degrees(-dir.y))._ * 0.5;
    auto bank = math::radians(math::degrees(dir.x))._ * 0.5;
    // Assuming the angles are in radians.
    double c1 = cos(heading);
    double s1 = sin(heading);
    double c2 = cos(attitude);
    double s2 = sin(attitude);
    double c3 = cos(bank);
    double s3 = sin(bank);
    double c1c2 = c1 * c2;
    double s1s2 = s1 * s2;
    auto w = static_cast<float>(c1c2 * c3 - s1s2 * s3);
    auto x = static_cast<float>(c1c2 * s3 + s1s2 * c3);
    auto y = static_cast<float>(s1 * c2 * c3 + c1 * s2 * s3);
    auto z = static_cast<float>(c1 * s2 * c3 - s1 * c2 * s3);

    glm::quat baseRotation = glm::quat(x,y,z,w);

    //Building rotations
    heading = math::radians(math::degrees(rx))._ * 0.5;
    attitude = math::radians(math::degrees(ry))._ * 0.5;
    bank = math::radians(math::degrees(rx))._ * 0.5;
    // Assuming the angles are in radians.
    c1 = cos(heading);
    s1 = sin(heading);
    c2 = cos(attitude);
    s2 = sin(attitude);
    c3 = cos(bank);
    s3 = sin(bank);
    c1c2 = c1 * c2;
    s1s2 = s1 * s2;
    w = static_cast<float>(c1c2 * c3 - s1s2 * s3);
    x = static_cast<float>(c1c2 * s3 + s1s2 * c3);
    y = static_cast<float>(s1 * c2 * c3 + c1 * s2 * s3);
    z = static_cast<float>(c1 * s2 * c3 - s1 * c2 * s3);

    glm::quat newRotation = glm::quat(x, y, z, w);
    glm::quat finalRotation = baseRotation * newRotation;
    glm::quat finalRotationNormalized = glm::normalize(finalRotation);

    auto eulerAngles = glm::eulerAngles(finalRotationNormalized);
    dir.x = math::degrees(math::radians(eulerAngles.z))._;
    dir.y = math::degrees(math::radians(eulerAngles.x))._;
    dir.z = math::degrees(math::radians(eulerAngles.y))._;

    obj->recalcExtents();

    updateTilesEntry(entry, model_update::add);
  }
}

void World::rotate_model_to_ground_normal(SceneObject* obj, bool smoothNormals)
{
    NOGGIT_CUR_ACTION->registerObjectTransformed(obj);

    updateTilesEntry(obj, model_update::remove);

    glm::vec3 rayPos = obj->pos;
    math::degrees::vec3& dir = obj->dir;


    selection_result results;
    // Same ray, same fix as World::get_ground_height below: the origin goes one unit above this
    // chunk's highest vertex instead of sitting at the object's own position. The old
    // "we shouldn't end up with empty ever (but we do, on completely flat ground)" TODO that used
    // to sit under this block was that bug describing itself -- an object resting on flat terrain
    // put the ray origin in the plane of the triangle it was standing on, where the intersection
    // is degenerate, and the upward retry had nothing above it to hit either.
    for_maybe_chunk_at(rayPos, [&](MapChunk* chunk)
        {
            float const ray_height (std::max(rayPos.y, chunk->getMaxHeight()) + 1.0f);
            math::ray intersect_ray
                ( glm::vec3(rayPos.x, ray_height, rayPos.z)
                , glm::vec3(0.f, -1.f, 0.f)
                );
            chunk->intersect(intersect_ray, &results, true);

            return true;
        });

    if (results.empty())
    {
        // Still reachable, for an unloaded tile. Leaving the rotation alone is the right answer:
        // there is no ground normal to align to, so there is nothing to align.
        updateTilesEntry(obj, model_update::add);
        return;
    }


    // We hit the terrain, now we take the normal of this position and use it to get the rotation we want.
    auto const& hitChunkInfo = std::get<selected_chunk_type>(results.front().second);

    glm::quat q;
    glm::vec3 varnormal;

    // Surface Normal
    auto& p0 = hitChunkInfo.chunk->mVertices[std::get<0>(hitChunkInfo.triangle)];
    auto& p1 = hitChunkInfo.chunk->mVertices[std::get<1>(hitChunkInfo.triangle)];
    auto& p2 = hitChunkInfo.chunk->mVertices[std::get<2>(hitChunkInfo.triangle)];

    glm::vec3 v1 = p1 - p0;
    glm::vec3 v2 = p2 - p0;

    auto tmpVec = glm::cross(v2, v1);
    varnormal.x = tmpVec.z;
    varnormal.y = tmpVec.y;
    varnormal.z = tmpVec.x;

    // Smooth option, gradient the normal towards closest vertex
    if (smoothNormals) // Vertex Normal
    {
        auto normalWeights = getBarycentricCoordinatesAt(p0, p1, p2, hitChunkInfo.position, varnormal);

        auto& tile_buffer = hitChunkInfo.chunk->mt->getChunkHeightmapBuffer();
        int chunk_start = (hitChunkInfo.chunk->px * 16 + hitChunkInfo.chunk->py) * mapbufsize * 4;

        const auto& vNormal0 = *reinterpret_cast<glm::vec3*>(&tile_buffer[chunk_start + std::get<0>(hitChunkInfo.triangle) * 4]);
        const auto& vNormal1 = *reinterpret_cast<glm::vec3*>(&tile_buffer[chunk_start + std::get<1>(hitChunkInfo.triangle) * 4]);
        const auto& vNormal2 = *reinterpret_cast<glm::vec3*>(&tile_buffer[chunk_start + std::get<2>(hitChunkInfo.triangle) * 4]);

        varnormal.x =
            vNormal0.x * normalWeights.x +
            vNormal1.x * normalWeights.y +
            vNormal2.x * normalWeights.z;

        varnormal.y =
            vNormal0.y * normalWeights.x +
            vNormal1.y * normalWeights.y +
            vNormal2.y * normalWeights.z;

        varnormal.z =
            vNormal0.z * normalWeights.x +
            vNormal1.z * normalWeights.y +
            vNormal2.z * normalWeights.z;
    }


    glm::vec3 worldUp = glm::vec3(0, 1, 0);
    glm::vec3 a = glm::cross(worldUp, varnormal);

    q.x = a.x;
    q.y = a.y;
    q.z = a.z;

    auto worldLengthSqrd = glm::length(worldUp) * glm::length(worldUp);
    auto normalLengthSqrd = glm::length(varnormal) * glm::length(varnormal);
    auto worldDotNormal = glm::dot(worldUp, varnormal);

    q.w = std::sqrt((worldLengthSqrd * normalLengthSqrd) + (worldDotNormal));

    auto normalizedQ = glm::normalize(q);

    //math::degrees::vec3 new_dir;
    // To euler, because wow
      /*
      // roll (x-axis rotation)
      double sinr_cosp = 2.0 * (q.w * q.x + q.y * q.z);
      double cosr_cosp = 1.0 - 2.0 * (q.x * q.x + q.y * q.y);
      new_dir.z = std::atan2(sinr_cosp, cosr_cosp) * 180.0f / math::constants::pi;

      // pitch (y-axis rotation)
      double sinp = 2.0 * (q.w * q.y - q.z * q.x);
      if (std::abs(sinp) >= 1)
        new_dir.y = std::copysign(math::constants::pi / 2, sinp) * 180.0f / math::constants::pi; // use 90 degrees if out of range
      else
        new_dir.y = std::asin(sinp) * 180.0f / math::constants::pi;

      // yaw (z-axis rotation)
      double siny_cosp = 2.0 * (q.w * q.z + q.x * q.y);
      double cosy_cosp = 1.0 - 2.0 * (q.y * q.y + q.z * q.z);
      new_dir.x = std::atan2(siny_cosp, cosy_cosp) * 180.0f / math::constants::pi;
     }*/

    auto eulerAngles = glm::eulerAngles(normalizedQ);
    dir.x = math::degrees(math::radians(eulerAngles.z))._; //Roll
    dir.y = math::degrees(math::radians(eulerAngles.x))._; //Pitch
    dir.z = math::degrees(math::radians(eulerAngles.y))._; //Yaw

    obj->recalcExtents();

    // yaw (z-axis rotation)
    double siny_cosp = 2 * (q.w * q.z + q.x * q.y);
    double cosy_cosp = 1 - 2 * (q.y * q.y + q.z * q.z);
    updateTilesEntry(obj, model_update::add);
}

void World::rotate_selected_models_to_ground_normal(bool smoothNormals)
{
  ZoneScoped;
  if (!_selected_model_count)
      return;

  for (auto& entry : _current_selection)
  {
    auto type = entry.index();
    if (type == eEntry_MapChunk)
    {
      continue;
    }

    auto& obj = std::get<selected_object_type>(entry);

    rotate_model_to_ground_normal(obj, smoothNormals);
  }
  update_selected_model_groups();
}

void World::set_current_selection(selection_type entry)
{
  ZoneScoped;
  reset_selection();
  add_to_selection(entry);
}

// updating pivot is expensive, in mass selection situation, it should only be updated once after operation is done
// now checks if model is already selected, don't need to call is_selected anymore !
bool World::add_to_selection(selection_type entry, bool skip_group, bool update_pivot)
{
  ZoneScoped;
  if (entry.index() == eEntry_Object)
  {

    auto obj = std::get<selected_object_type>(entry);

    auto result = selected_uids.insert(obj->uid);

    if (!result.second)
    {
      // Duplicate existed
      return false;
    }

    _selected_model_count++;
    // check if it is in a group
    if (!skip_group)
    {
        for (auto& group : _selection_groups)
        {
            if (group.contains_object(obj))
            {
                // make sure to add it to selection before donig group selection so it doesn't get selected twice
                _current_selection.push_back(entry);
                // this then calls add_to_selection() with skip_group = true to avoid repetition
                group.select_group();
                return true;
            }
        }
    }
  }
  _current_selection.push_back(entry);

  if (update_pivot)
    update_selection_pivot();

  return true;
}

void World::remove_from_selection(selection_type entry, bool skip_group, bool update_pivot)
{
  ZoneScoped;
  if (entry.index() == eEntry_Object)
  {
    auto obj = std::get<selected_object_type>(entry);
    size_t erased_count = selected_uids.erase(obj->uid);
    if (erased_count == 0)
      return;
  }

  std::vector<selection_type>::iterator position = std::find(_current_selection.begin(), _current_selection.end(), entry);
  if (position != _current_selection.end())
  {
    if (entry.index() == eEntry_Object)
    {
      _selected_model_count--;
      // check if it is in a group
      if (!skip_group)
      {
        for (auto& group : _selection_groups)
        {
          auto obj = std::get<selected_object_type>(entry);
          if (group.contains_object(obj))
          {
              // this then calls remove_from_selection() with skip_group = true to avoid repetition
              group.unselect_group();
              break;
          }
        }
      }
    }

    _current_selection.erase(position);
    if (update_pivot)
      update_selection_pivot();
  }
}

void World::remove_from_selection(std::uint32_t uid, bool skip_group, bool update_pivot)
{
  ZoneScoped;
  size_t erased_count = selected_uids.erase(uid);
  if (erased_count == 0)
    return;

  for (auto it = _current_selection.begin(); it != _current_selection.end(); ++it)
  {
    if (it->index() != eEntry_Object)
      continue;

    auto obj = std::get<selected_object_type>(*it);

    if (obj->uid == uid)
    {
      _selected_model_count--;
      _current_selection.erase(it);

      // check if it is in a group
      if (!skip_group)
      {
        for (auto& group : _selection_groups)
        {
          if (group.contains_object(obj))
          {
            // this then calls remove_from_selection() with skip_group = true to avoid repetition
            group.unselect_group();
            break;
          }
        }
      }
      if (update_pivot)
        update_selection_pivot();
      return;
    }

  }
}

void World::reset_selection()
{
  ZoneScoped;
  selected_uids.clear();
  _current_selection.clear();
  _multi_select_pivot = std::nullopt;
  _selected_model_count = 0;

  for (auto& selection_group : _selection_groups)
  {
      selection_group.setUnselected();
  }
}

void World::delete_selected_models()
{
  ZoneScoped;
  if (!_selected_model_count)
      return;

  // erase selected groups as well
  for (auto& group : _selection_groups)
  {
      if (group.isSelected())
      {
          group.remove_group();
      }
  }

  _model_instance_storage.delete_instances(get_selected_objects(), true);
  need_model_updates = true;
  reset_selection();
}

bool World::try_get_ground_height(glm::vec3 const& pos, float& out_height)
{
  glm::vec3 const ground (get_ground_height_impl(pos, nullptr));

  // The impl reports a miss by handing back the input, and that is unambiguous here because a hit
  // returns the intersection point, whose y is the SURFACE -- never the caller's own y except by
  // a coincidence that costs one frame of camera adjustment and self-corrects.
  if (ground == pos)
  {
    return false;
  }

  out_height = ground.y;
  return true;
}

glm::vec3 World::get_ground_height(glm::vec3 pos)
{
  bool missed (false);
  glm::vec3 const ground (get_ground_height_impl(pos, &missed));

  if (missed)
  {
    // Reported here and not in the impl, because this overload is the object-snap path: it runs on
    // a user gesture, so one line per failed snap is useful. try_get_ground_height is the camera's
    // path and runs every frame, where the same line would be a flood.
    LogError << "Snap to ground: no loaded terrain at (" << pos.x << ", " << pos.z
             << "), leaving the position unchanged" << std::endl;
  }

  return ground;
}

glm::vec3 World::get_ground_height_impl(glm::vec3 pos, bool* missed)
{
    selection_result hits;

    // for_maybe_chunk_at, not for_chunk_at: the latter calls mapIndex.setChanged(tile) on entry
    // (World.inl:53), and this function is a pure query. Two of its callers are the FPS camera and
    // camera collision in MapView::tick, which run it on every frame the camera moved -- so with
    // for_chunk_at, merely flying across the map marked every tile it passed over as needing a
    // save, and "Save changed tiles" then rewrote terrain nobody had edited. Everything that
    // actually MOVES something after asking for the height marks its own tiles: World::
    // updateTilesEntry reaches MapIndex::update_model_tile, which sets adt->changed on both the
    // remove and the add (map_index.cpp:353).
    for_maybe_chunk_at(pos, [&](MapChunk* chunk)
    {
        // The ray starts ABOVE the terrain, not at the caller's own y, and that is the whole fix.
        //
        // Every caller but the camera passes the position of the object being snapped, and an
        // object that is already standing on the ground puts the origin of a downward ray exactly
        // on -- or a float's width below -- the surface it is trying to find. The old code then
        // found nothing, retried upwards, found nothing there either because there is no terrain
        // above an object sitting on terrain, and reported no ground at all. On perfectly flat
        // ground the origin lies IN the triangle's plane, where ray/triangle intersection is
        // degenerate, which is why the identical ray in rotate_model_to_ground_normal above
        // carried a "we do end up empty on completely flat ground" note for years.
        //
        // getMaxHeight() is the chunk's own vmax.y (MapChunk.cpp:436-439), so one unit above it is
        // above all 145 of this chunk's vertices, at any terrain height, including below sea level
        // where y is negative. It is combined with pos.y rather than used alone because vmax is
        // not recomputed at the moment a vertex moves: MapChunk::changeTerrain and friends only
        // raise ChunkUpdateFlags::VERTEX, and vmin/vmax are rebuilt later in
        // MapChunk::updateVerticesData (MapChunk.cpp:639-663). The flatten and terrain tools call
        // this function immediately after a brush tick, so vmax can lag the terrain by a frame,
        // and taking the higher of the two starts the ray above both the stale bound and the
        // caller -- which can never be worse than either on its own.
        float const ray_height (std::max(pos.y, chunk->getMaxHeight()) + 1.0f);
        math::ray intersect_ray
            (glm::vec3(pos.x, ray_height, pos.z), glm::vec3(0.f, -1.f, 0.f));
        chunk->intersect(intersect_ray, &hits, true);

        // The upward retry the old code did here is gone with the reason for it. It existed to
        // catch "the object is below ground"; from above every vertex, below-ground is not a state
        // the ray can be in. MapChunk::intersect tests all 256 triangles of the chunk with no hole
        // check (MapChunk.cpp:617-636), so the column is covered wherever pos lands inside the
        // chunk -- and where an upward ray would have missed, so would this one.
        return true;
    });

    if (hits.empty())
    {
        // A miss now means the tile is not loaded, so there is no terrain to answer with. Return
        // what we were given. This half matters more than the ray origin: the old code returned
        // glm::vec3(0), and since callers take only .y from it, a failed snap did not decline to
        // move the object -- it moved it to world y = 0, which on most maps is under the sea or
        // under the terrain, and the object was gone. A failed snap must be a no-op.
        if (missed)
        {
            *missed = true;
        }

        return pos;
    }

    return std::get<selected_chunk_type>(hits[0].second).position;
}

void World::snap_selected_models_to_the_ground()
{
  ZoneScoped;
  if (!_selected_model_count)
      return;
  for (auto& entry : _current_selection)
  {
    auto type = entry.index();
    if (type == eEntry_MapChunk)
    {
      continue;
    }

    auto& obj = std::get<selected_object_type>(entry);
    NOGGIT_CUR_ACTION->registerObjectTransformed(obj);
    glm::vec3& pos = obj->pos;

    // the ground can only be intersected once
    pos.y = get_ground_height(pos).y;

    std::get<selected_object_type>(entry)->recalcExtents();

    updateTilesEntry(entry, model_update::add);
  }
  update_selection_pivot();
  update_selected_model_groups();
}

void World::scale_selected_models(float v, object_scaling_type type)
{
  ZoneScoped;
  if (!_selected_model_count)
      return;

  bool modern_features = Noggit::Application::NoggitApplication::instance()->getConfiguration()->modern_features;

  for (auto& entry : _current_selection)
  {
    if (entry.index() == eEntry_Object)
    {
      auto obj = std::get<selected_object_type>(entry);

      if (obj->which() != eMODEL)
      {
          // If we are not using modern features, we don't want to scale WMOs
        if(!modern_features)
			    continue;

        WMOInstance* wi = static_cast<WMOInstance*>(obj);

        NOGGIT_CUR_ACTION->registerObjectTransformed(wi);

        float scale = wi->scale;

        switch (type)
        {
        case World::object_scaling_type::set:
            scale = v;
            break;
        case World::object_scaling_type::add:
            scale += v;
            break;
        case World::object_scaling_type::mult:
            scale *= v;
            break;
        }

        // if the change is too small, do nothing
        if (std::abs(scale - wi->scale) < ModelInstance::min_scale())
        {
            continue;
        }

        updateTilesWMO(wi, model_update::remove);
        wi->scale = std::min(ModelInstance::max_scale(), std::max(ModelInstance::min_scale(), scale));
        wi->recalcExtents();
        updateTilesWMO(wi, model_update::add);
      }
      else
      {
        ModelInstance* mi = static_cast<ModelInstance*>(obj);

        NOGGIT_CUR_ACTION->registerObjectTransformed(mi);

        float scale = mi->scale;

        switch (type)
        {
        case World::object_scaling_type::set:
            scale = v;
            break;
        case World::object_scaling_type::add:
            scale += v;
            break;
        case World::object_scaling_type::mult:
            scale *= v;
            break;
        }

        // if the change is too small, do nothing
        if (std::abs(scale - mi->scale) < ModelInstance::min_scale())
        {
            continue;
        }

        updateTilesModel(mi, model_update::remove);
        mi->scale = std::min(ModelInstance::max_scale(), std::max(ModelInstance::min_scale(), scale));
        mi->recalcExtents();
        updateTilesModel(mi, model_update::add);
      }
    }
  }
  update_selected_model_groups();
}

void World::move_selected_models(float dx, float dy, float dz)
{
  ZoneScoped;
  if (!_selected_model_count)
      return;
  for (auto& entry : _current_selection)
  {
    auto type = entry.index();
    if (type == eEntry_MapChunk)
    {
      continue;
    }

    auto& obj = std::get<selected_object_type>(entry);
    NOGGIT_CUR_ACTION->registerObjectTransformed(obj);
    glm::vec3& pos = obj->pos;

    updateTilesEntry(entry, model_update::remove);

    pos.x += dx;
    pos.y += dy;
    pos.z += dz;

    std::get<selected_object_type>(entry)->recalcExtents();

    updateTilesEntry(entry, model_update::add);
  }
  update_selection_pivot();
  update_selected_model_groups();
}

void World::move_model(selection_type entry, float dx, float dy, float dz)
{
    ZoneScoped;
    auto type = entry.index();
    if (type == eEntry_MapChunk)
    {
        return;
    }

    auto& obj = std::get<selected_object_type>(entry);
    NOGGIT_CUR_ACTION->registerObjectTransformed(obj);
    glm::vec3& pos = obj->pos;

    updateTilesEntry(entry, model_update::remove);

    pos.x += dx;
    pos.y += dy;
    pos.z += dz;

    std::get<selected_object_type>(entry)->recalcExtents();

    updateTilesEntry(entry, model_update::add);

}

void World::move_selected_models(glm::vec3 const& delta)
{
  move_selected_models(delta.x, delta.y, delta.z);
}

void World::set_selected_models_pos(float x, float y, float z, bool change_height)
{
  return set_selected_models_pos({ x,y,z }, change_height);
}

void World::set_selected_models_pos(glm::vec3 const& pos, bool change_height)
{
  ZoneScoped;
  if (!_selected_model_count)
      return;
  // move models relative to the pivot when several are selected
  if (has_multiple_model_selected())
  {
    glm::vec3 diff = pos - _multi_select_pivot.value();

    if (change_height)
    {
      move_selected_models(diff);
    }
    else
    {
      move_selected_models(diff.x, 0.f, diff.z);
    }

    return;
  }

  for (auto& entry : _current_selection)
  {
    auto type = entry.index();
    if (type == eEntry_MapChunk)
    {
      continue;
    }

    updateTilesEntry(entry, model_update::remove);

    auto& obj = std::get<selected_object_type>(entry);
    NOGGIT_CUR_ACTION->registerObjectTransformed(obj);
    obj->pos = pos;
    obj->recalcExtents();

    updateTilesEntry(entry, model_update::add);
  }
  update_selection_pivot();
  update_selected_model_groups();
}

void World::set_model_pos(selection_type entry, glm::vec3 const& pos, bool change_height)
{
  ZoneScoped;
  auto type = entry.index();
  if (type == eEntry_MapChunk)
  {
      return;
  }
  
  updateTilesEntry(entry, model_update::remove);
  
  auto& obj = std::get<selected_object_type>(entry);
  NOGGIT_CUR_ACTION->registerObjectTransformed(obj);
  obj->pos = pos;
  obj->recalcExtents();
  
  updateTilesEntry(entry, model_update::add);
}

void World::rotate_selected_models(math::degrees rx, math::degrees ry, math::degrees rz, bool use_pivot)
{
  ZoneScoped;
  if (!_selected_model_count)
      return;

  math::degrees::vec3 dir_change(rx._, ry._, rz._);
  bool has_multi_select = has_multiple_model_selected();

  for (auto& entry : _current_selection)
  {
    auto type = entry.index();
    if (type == eEntry_MapChunk)
    {
      continue;
    }

    updateTilesEntry(entry, model_update::remove);

    auto& obj = std::get<selected_object_type>(entry);
    NOGGIT_CUR_ACTION->registerObjectTransformed(obj);

    if (use_pivot && has_multi_select)
    {
      glm::vec3& pos = obj->pos;
      math::degrees::vec3& dir = obj->dir;
      glm::vec3 diff_pos = pos - _multi_select_pivot.value();

      glm::quat rotationQuat = glm::quat(glm::vec3(glm::radians(rx._), glm::radians(ry._), glm::radians(rz._)));
      glm::vec3 rot_result = glm::toMat4(rotationQuat) * glm::vec4(diff_pos,0);

      pos += rot_result - diff_pos;
    }
    else
    {
      // math::degrees::vec3& dir = obj->dir;
      // dir += dir_change;
    }
    math::degrees::vec3& dir = obj->dir;
    dir += dir_change;

    obj->recalcExtents();

    updateTilesEntry(entry, model_update::add);
  }
  update_selected_model_groups();
}

void World::set_selected_models_rotation(math::degrees rx, math::degrees ry, math::degrees rz)
{
  ZoneScoped;
  if (!_selected_model_count)
      return;

  math::degrees::vec3 new_dir(rx._, ry._, rz._);

  for (auto& entry : _current_selection)
  {
    auto type = entry.index();
    if (type != eEntry_Object)
    {
      continue;
    }

    auto& obj = std::get<selected_object_type>(entry);
    NOGGIT_CUR_ACTION->registerObjectTransformed(obj);

    updateTilesEntry(entry, model_update::remove);

    math::degrees::vec3& dir = obj->dir;

    dir = new_dir;

    obj->recalcExtents();

    updateTilesEntry(entry, model_update::add);
  }
  update_selected_model_groups();
}

void World::update_selected_model_groups()
{
  for (auto& selection_group : _selection_groups)
  {
      if (selection_group.isSelected())
          selection_group.recalcExtents();
  }
}

MapChunk* World::getChunkAt(glm::vec3 const& pos)
{
  MapTile* tile(mapIndex.getTile(pos));
  if (tile && tile->finishedLoading())
  {
    return tile->getChunk((pos.x - tile->xbase) / CHUNKSIZE, (pos.z - tile->zbase) / CHUNKSIZE);
  }
  return nullptr;
}

bool World::isInIndoorWmoGroup(std::array<glm::vec3, 2> obj_bounds, glm::mat4x4 obj_transform)
{
    bool is_indoor = false;
    // check if model bounds is within wmo bounds then check each indor wmo group bounds
    _model_instance_storage.for_each_wmo_instance([&](WMOInstance& wmo_instance)
        {
            auto wmo_extents = wmo_instance.getExtents();
            // check if global wmo bounds intersect
            if (obj_bounds[1].x >= wmo_extents[0].x
                && obj_bounds[1].y >= wmo_extents[0].y
                && obj_bounds[1].z >= wmo_extents[0].z
                && wmo_extents[1].x >= obj_bounds[0].x
                && wmo_extents[1].y >= obj_bounds[0].y
                && wmo_extents[1].z >= obj_bounds[0].z)

            {
                for (int i = 0; i < (int)wmo_instance.wmo->groups.size(); ++i)
                {
                    auto const& group = wmo_instance.wmo->groups[i];

                    if (group.is_indoor())
                    {
                        // must call getGroupExtent() to initialize wmo_instance.group_extents
                        // clear group extents to free memory ?
                        auto& group_extents = wmo_instance.getGroupExtents().at(i);


                        bool aabb_test = obj_bounds[1].x >= group_extents.first.x
                            && obj_bounds[1].y >= group_extents.first.y
                            && obj_bounds[1].z >= group_extents.first.z
                            && group_extents.second.x >= obj_bounds[0].x
                            && group_extents.second.y >= obj_bounds[0].y
                            && group_extents.second.z >= obj_bounds[0].z;

                        // TODO : do a precise calculation instead of using axis aligned bounding boxes.
                        if (aabb_test) // oriented box check
                        {
                            /* TODO
                            if (collide_test)
                            {
                                is_indoor = true;
                                return;
                            }
                            */
                        }
                    }
                }
            }
        });

    return is_indoor;
}

selection_result World::intersect (glm::mat4x4 const& model_view
                                  , math::ray const& ray
                                  , bool pOnlyMap
                                  , bool do_objects
                                  , bool draw_terrain
                                  , bool draw_wmo
                                  , bool draw_models
                                  , bool draw_hidden_models
                                  , bool draw_wmo_exterior
                                  , bool animate
                                  )
{
  ZoneScopedN("World::intersect()");
  selection_result results;

  if (draw_terrain)
  {
    ZoneScopedN("World::intersect() : intersect terrain");

    for (auto& pair : _loaded_tiles_buffer)
    {
      MapTile* tile = pair.second;

      if (!tile)
        break;

      TileIndex index{ static_cast<std::size_t>(pair.first.first)
                        , static_cast<std::size_t>(pair.first.second) };

      // handle tiles that got unloaded mid-frame to avoid illegal access
      if (!mapIndex.tileLoaded(index) || mapIndex.tileAwaitingLoading(index))
          continue;

      if (!tile->finishedLoading())
        continue;

      if (tile->intersect(ray, &results))
        break;
    }
  }

  if (!pOnlyMap && do_objects)
  {
    if (draw_models)
    {
      ZoneScopedN("World::intersect() : intersect M2s");
      _model_instance_storage.for_each_m2_instance([&] (ModelInstance& model_instance)
      {
        if (draw_hidden_models || !model_instance.model->is_hidden())
        {
          model_instance.intersect(model_view, ray, &results, animtime, animate);
        }
      });
    }

    if (draw_wmo)
    {
      ZoneScopedN("World::intersect() : intersect WMOs");
      _model_instance_storage.for_each_wmo_instance([&] (WMOInstance& wmo_instance)
      {
        if (draw_hidden_models || !wmo_instance.wmo->is_hidden())
        {
          wmo_instance.intersect(ray, &results, draw_wmo_exterior);
        }
      });
    }
  }

  return std::move(results);
}

void World::update_models_emitters(float dt)
{
  ZoneScoped;
  while (dt > 0.1f)
  {
    ModelManager::updateEmitters(0.1f);
    dt -= 0.1f;
  }
  ModelManager::updateEmitters(dt);
}

unsigned int World::getAreaID (glm::vec3 const& pos)
{
  ZoneScoped;
  return for_maybe_chunk_at (pos, [&] (MapChunk* chunk) { return chunk->getAreaID(); }).value_or(-1);
}

void World::clearHeight(glm::vec3 const& pos)
{
  ZoneScoped;
  for_all_chunks_on_tile(pos, [](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
    chunk->clearHeight();
  });
  for_all_chunks_on_tile(pos, [this] (MapChunk* chunk) {
      recalc_norms (chunk);
  });
}

void World::clearAllModelsOnADT(TileIndex const& tile, bool action)
{
  ZoneScoped;
  _model_instance_storage.delete_instances_from_tile(tile, action);
  // update_models_by_filename();
}

void World::CropWaterADT(const TileIndex& pos)
{
  ZoneScoped;
  for_tile_at(pos, [](MapTile* tile)
  {
    for (int i = 0; i < 16; ++i)
      for (int j = 0; j < 16; ++j)
        NOGGIT_CUR_ACTION->registerChunkLiquidChange(tile->getChunk(i, j));

    tile->CropWater();
  });
}

void World::setAreaID(glm::vec3 const& pos, int id, bool adt, float radius)
{
  ZoneScoped;
  if (adt)
  {
    for_all_chunks_on_tile(pos, [&](MapChunk* chunk)
    {
      NOGGIT_CUR_ACTION->registerChunkAreaIDChange(chunk);
      chunk->setAreaID(id);
    });
  }
  else
  {

    if (radius >= 0)
    {
      for_all_chunks_in_range(pos, radius,
                              [&] (MapChunk* chunk)
                              {
                                NOGGIT_CUR_ACTION->registerChunkAreaIDChange(chunk);
                                chunk->setAreaID(id);
                                return true;
                              }
      );

    }
    else
    {
      for_chunk_at(pos, [&](MapChunk* chunk)
      {
        NOGGIT_CUR_ACTION->registerChunkAreaIDChange(chunk);
        chunk->setAreaID(id);
      });
    }
  }
}

Noggit::NoggitRenderContext World::getRenderContext() const
{
  return _context;
}

bool World::GetVertex(float x, float z, glm::vec3 *V) const
{
  ZoneScoped;
  TileIndex tile({x, 0, z});

  if (!mapIndex.tileLoaded(tile))
  {
    return false;
  }

  MapTile* adt = mapIndex.getTile(tile);

  return adt->GetVertex(x, z, V);
}



void World::changeShader(glm::vec3 const& pos, glm::vec4 const& color, float change, float radius, bool editMode)
{
  ZoneScoped;
  for_all_chunks_in_range
    ( pos, radius
    , [&] (MapChunk* chunk)
      {
        NOGGIT_CUR_ACTION->registerChunkVertexColorChange(chunk);
        return chunk->ChangeMCCV(pos, color, change, radius, editMode);
      }
    );
}

void World::stampShader(glm::vec3 const& pos, glm::vec4 const& color, float change, float radius, bool editMode, QImage* img, bool paint, bool use_image_colors)
{
  ZoneScoped;
  for_all_chunks_in_rect
    ( pos, radius
      , [&] (MapChunk* chunk)
      {
        NOGGIT_CUR_ACTION->registerChunkVertexColorChange(chunk);
        return chunk->stampMCCV(pos, color, change, radius, editMode, img, paint, use_image_colors);
      }
    );
}

glm::vec3 World::pickShaderColor(glm::vec3 const& pos)
{
  ZoneScoped;
  glm::vec3 color = glm::vec3(1.0f, 1.0f, 1.0f);
  for_all_chunks_in_range
  (pos, 0.1f
    , [&] (MapChunk* chunk)
  {
    color = chunk->pickMCCV(pos);
    return true;
  }
  );

  return color;
}

auto World::stamp(glm::vec3 const& pos, float dt, QImage const* img, float radiusOuter
, float radiusInner, int brushType, bool sculpt) -> void
{
  ZoneScoped;
  auto action = NOGGIT_CUR_ACTION;
  float delta = action->getDelta() + dt;
  action->setDelta(delta);

  for_all_chunks_in_rect(pos, radiusOuter,
                          [=](MapChunk* chunk) -> bool
                          {
                            auto action = NOGGIT_CUR_ACTION;
                            action->registerChunkTerrainChange(chunk);
                            action->setBlockCursor(!sculpt);
                            chunk->stamp(pos, dt, img, radiusOuter, radiusInner, brushType, sculpt); return true;
                          }
                          , [this](MapChunk* chunk) -> void
                          {
                            recalc_norms(chunk);

                            // check if coord axis > 0
                            // if true, get chunk by coord axis - 1
                            // else, check if tile coord axis > 0
                            // if true, get tile by tile coord axis - 1,  get last chunk by axis
                            auto get_neighbor =
                              [this, chunk](int px, int py) -> MapChunk*
                              {
                                MapChunk* neighbor{};

                                int new_chunk_x = px + chunk->px;
                                int new_chunk_z = py + chunk->py;

                                if (new_chunk_x < 0 || new_chunk_z < 0 || new_chunk_x == 16 || new_chunk_z == 16)
                                {
                                  TileIndex index(chunk->mt->index.x + px, chunk->mt->index.z + py);
                                  if (index.x != std::numeric_limits<std::size_t>::max()
                                  && index.z != std::numeric_limits<std::size_t>::max()
                                  && index.x != 64
                                  && index.z != 64)
                                  {
                                    MapTile* neighbor_tile = mapIndex.getTile(index);

                                    if (!neighbor_tile)
                                      return nullptr;

                                    neighbor = neighbor_tile->getChunk((new_chunk_x + 16) % 16,
                                                                       (new_chunk_z + 16) % 16);
                                  }
                                }
                                else
                                {
                                  neighbor = chunk->mt->getChunk(new_chunk_x, new_chunk_z);
                                }

                                return neighbor;
                              };

                            if (auto neighbor = get_neighbor(-1, 0); neighbor)
                              chunk->fixGapLeft(neighbor);

                            if (auto neighbor = get_neighbor(0, -1); neighbor)
                              chunk->fixGapAbove(neighbor);

                            if (auto neighbor = get_neighbor(1, 0); neighbor)
                              neighbor->fixGapLeft(chunk);

                            if (auto neighbor = get_neighbor(0, 1); neighbor)
                              neighbor->fixGapAbove(chunk);

                          });
}


void World::changeObjectsWithTerrain(glm::vec3 const& pos, float change, float radius, int BrushType, float inner_radius, bool iter_wmos_, bool iter_m2s, bool follow_normals)
{
    // applies the terrain brush to the terrain objects hit
    ZoneScoped;

    if (!iter_wmos_ && !iter_m2s)
        return;

  // Identical code to chunk->changeTerrain()
  //    if (_snap_m2_objects_chkbox->isChecked() || _snap_wmo_objects_chkbox->isChecked()) {
  auto objects_hit = getObjectsInRange(pos, radius, true, iter_wmos_, iter_m2s);

  for (auto obj : objects_hit)
  {

    float dt = change;

    float dist, xdiff, zdiff;
    bool changed = false;

    xdiff = obj->pos.x - pos.x;
    zdiff = obj->pos.z - pos.z;

    if (BrushType == eTerrainType_Quadra)
    {
        if ((std::abs(xdiff) < std::abs(radius / 2)) && (std::abs(zdiff) < std::abs(radius / 2)))
        {
            dist = std::sqrt(xdiff * xdiff + zdiff * zdiff);
            dt = dt * (1.0f - dist * inner_radius / radius);
            changed = true;
        }
    }
    else
    {
        dist = std::sqrt(xdiff * xdiff + zdiff * zdiff);
        if (dist < radius)
        {
            changed = true;

            switch (BrushType)
            {
            case eTerrainType_Flat:
                break;
            case eTerrainType_Linear:
                dt = dt * (1.0f - dist * (1.0f - inner_radius) / radius);
                break;
            case eTerrainType_Smooth:
                dt = dt / (1.0f + dist / radius);
                break;
            case eTerrainType_Polynom:
                dt = dt * ((dist / radius) * (dist / radius) + dist / radius + 1.0f);
                break;
            case eTerrainType_Trigo:
                dt = dt * cos(dist / radius);
                break;
            case eTerrainType_Gaussian:
                dt = dist < radius * inner_radius ? dt * std::exp(-(std::pow(radius * inner_radius / radius, 2) / (2 * std::pow(0.39f, 2)))) : dt * std::exp(-(std::pow(dist / radius, 2) / (2 * std::pow(0.39f, 2))));

                break;
            default:
                LogError << "Invalid terrain edit type (" << inner_radius << ")" << std::endl;
                changed = false;
                break;
            }
        }
    }
    if (changed)
    {
        move_model(obj, 0.0f, dt, 0.0f);
        // set_model_pos(obj, glm::vec3(obj->pos.x, obj->pos.y + dt, obj->pos.z));

        // OBJECTS FOLLOW THE GROUND NORMAL, on the same opt-in switch that governs the height
        // snap above. This runs AFTER World::changeTerrain has already moved the vertices -- the
        // caller in TerrainTool::changeTerrain does the terrain first and this second -- so the
        // ray inside rotate_model_to_ground_normal reads the slope the object is standing on
        // NOW, not the one it was standing on when the stroke began.
        //
        // Safe to repeat every tick of a held stroke: rotate_model_to_ground_normal recomputes
        // `dir` from the measured normal rather than adding to it, so sixty calls a second
        // converge on the same answer instead of accumulating a spin.
        //
        // `true` for smooth normals, matching the default of the manual repair this replaces --
        // the Object Editor's "rotate along ground" starts with its smooth box ticked
        // (ObjectTool.hpp:82). A face normal makes a doodad step between the two triangles of a
        // terrain quad as the ground moves under it; the barycentric blend does not.
        if (follow_normals)
        {
            rotate_model_to_ground_normal(obj, true);
        }
    }
  }
}

void World::changeTerrain(glm::vec3 const& pos, float change, float radius, int BrushType, float inner_radius)
{
  ZoneScoped;

  for_all_chunks_in_range
    ( pos, radius
    , [&] (MapChunk* chunk)
      {
        NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
        return chunk->changeTerrain(pos, change, radius, BrushType, inner_radius);
      }
    , [this] (MapChunk* chunk)
      {
        recalc_norms (chunk);
      }
    );
}

std::vector<selected_object_type> World::getObjectsInRange(glm::vec3 const& pos, float radius, bool ignore_height, bool iter_wmos_, bool iter_m2s)
{
    // ignores height by default

    std::vector<selected_object_type> objects_hit_list;

    /* This causes duplicates at tile edges
    for (MapTile* tile : mapIndex.tiles_in_range(pos, radius))
    {
        if (!tile->finishedLoading())
        {
            continue;
        }

        std::vector<uint32_t>* uids = tile->get_uids();

        for (uint32_t uid : *uids)
        {
            auto instance = _model_instance_storage.get_instance(uid);

            */
    if (iter_m2s)
    {
        _model_instance_storage.for_each_m2_instance([&](ModelInstance& model_instance)
            {
                selected_object_type obj = &model_instance;
                auto obj_pos = obj->pos;
                if (ignore_height)
                {
                    obj_pos = glm::vec3(obj->pos.x, pos.y, obj->pos.z);
                }
                if (glm::distance(obj_pos, pos) <= radius) // this is just origin point
                {
                    objects_hit_list.push_back(obj);
                }
            });
    }

    if (iter_wmos_)
    {
        _model_instance_storage.for_each_wmo_instance([&](WMOInstance& wmo_instance)
            {
                selected_object_type obj = &wmo_instance;
                auto obj_pos = obj->pos;
                if (ignore_height)
                {
                    obj_pos = glm::vec3(obj->pos.x, pos.y, obj->pos.z);
                }
                if (glm::distance(obj_pos, pos) <= radius)
                {
                    objects_hit_list.push_back(obj);
                }
            });
    }

    return objects_hit_list;
}

void World::flattenTerrain(glm::vec3 const& pos, float remain, float radius, int BrushType, flatten_mode const& mode, const glm::vec3& origin, math::degrees angle, math::degrees orientation)
{
  ZoneScoped;
  for_all_chunks_in_range
    ( pos, radius
    , [&] (MapChunk* chunk)
      {
        NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
        return chunk->flattenTerrain(pos, remain, radius, BrushType, mode, origin, angle, orientation);
      }
    , [this] (MapChunk* chunk)
      {
        recalc_norms (chunk);
      }
    );
}

std::vector<std::pair<SceneObject*, float>> World::getObjectsGroundDistance(glm::vec3 const& pos, float radius, bool iter_wmos_, bool iter_m2s)
{
    std::vector<std::pair<SceneObject*, float>> objects_ground_distance;

    if (!iter_wmos_ && !iter_m2s)
        return objects_ground_distance;

    auto objects_hit = getObjectsInRange(pos, radius, true
        , iter_wmos_, iter_m2s);

    for (auto obj : objects_hit)
    {
        if ((obj->which() == eMODEL && !iter_m2s) || (obj->which() == eWMO && !iter_wmos_))
            continue;
        float height_diff = obj->pos.y - get_ground_height(obj->pos).y;
        objects_ground_distance.push_back(std::pair<SceneObject*, float>(obj, height_diff));
    }

    return objects_ground_distance;
}

void World::reseatObjectsOnGround(std::vector<std::pair<SceneObject*, float>> const& ground_distances, bool follow_normals)
{
    ZoneScoped;

    for (auto const& pair : ground_distances)
    {
        SceneObject* obj = pair.first;

        // get_ground_height returns `pos` unchanged when no loaded chunk answers, so a miss here
        // re-seats the object at the height it already has -- a no-op, which is the right answer
        // for a snap. See the long note on World::try_get_ground_height for why that matters.
        float const new_ground_height (get_ground_height(obj->pos).y);
        set_model_pos(obj, glm::vec3(obj->pos.x, new_ground_height + pair.second, obj->pos.z));

        // The re-tilt goes after the move, not before, only because that is the order the two
        // operations are independent in: rotate_model_to_ground_normal casts its ray from above
        // the chunk's highest vertex straight down, so the object's own y does not enter into
        // which triangle it finds. Doing it second keeps the object's final position and final
        // rotation derived from the same terrain state.
        if (follow_normals)
        {
            rotate_model_to_ground_normal(obj, true);
        }
    }
}

namespace
{
    //! How much of the ramp's target height a vertex takes.
    //!
    //! `outside` is the shortest distance, in the XZ plane and in yards, from the vertex to the
    //! ramp's flat core -- zero anywhere inside it. 1 in the core, 0 at and beyond the falloff
    //! band, and the chosen curve in between.
    float rampFalloff(float outside, float falloff_width, bool smooth)
    {
        if (outside <= 0.0f)
        {
            return 1.0f;
        }

        // A zero-width band is a cliff by request: the core takes the full height and everything
        // outside it keeps the terrain it had. Guarded before the division rather than clamped
        // after it, because outside/0 is inf and inf compares its way into neither branch.
        if (falloff_width <= 0.0f || outside >= falloff_width)
        {
            return 0.0f;
        }

        float const s (1.0f - outside / falloff_width);

        // Smoothstep, 3s^2 - 2s^3. Its derivative is 0 at both s = 0 and s = 1, so the surface
        // meets the untouched terrain at the outer edge of the band AND the flat core at the
        // inner edge with no crease at either. The straight line has a slope discontinuity at
        // both, which is the visible fold along the side of a road and the whole reason there are
        // two curves to pick from rather than one.
        return smooth ? s * s * (3.0f - 2.0f * s) : s;
    }
}

World::RampProjection World::projectOntoRamp(glm::vec3 const& start, glm::vec3 const& end, float width, glm::vec3 const& pos)
{
    // THE RAMP'S FOOTPRINT, in one paragraph. Everything is in the XZ plane, against the segment
    // from `start` to `end`. `t` is how far along that segment the point projects and `side` how
    // far it lies off the centre line. The flat core is the rectangle t in [0, length] and
    // side <= width/2; the distance returned is the exact 2-D distance from the point to that
    // rectangle, which is the hypotenuse of the two overshoots -- the standard point-to-box
    // distance. That is what gives the falloff ROUNDED corners at the ramp's four corners. Square
    // corners would put two folds meeting at a point, which is exactly the seam a mapper building
    // a road is trying to get away from.
    //
    // `along` is clamped, so past either end the ramp aims at that end's height rather than
    // extrapolating. Both ends are terrain picks, so the ramp already meets the ground exactly at
    // its two ends along the centre line; the falloff band -- which extends PAST each end as well
    // as to each side, because the box distance treats the rectangle's short edges no differently
    // from its long ones -- carries that join outward across the full width.
    float const dx (end.x - start.x);
    float const dz (end.z - start.z);
    float const length (std::sqrt(dx * dx + dz * dz));

    RampProjection projection;

    if (length < 0.001f)
    {
        // A degenerate ramp reports every point as infinitely far OUTSIDE it, not as sitting in
        // the middle of it. The struct's own default is zero, which reads as "in the core", and a
        // caller that got that would treat the whole world as inside a ramp with no length.
        // buildRamp refuses this case before it ever calls here, but the object filter in
        // flatten_blur_tool::buildRamp calls it first, so the safe answer has to be the one here.
        projection.distance_to_core = std::numeric_limits<float>::max();
        return projection;
    }

    float const ux (dx / length);
    float const uz (dz / length);

    float const ox (pos.x - start.x);
    float const oz (pos.z - start.z);

    float const t (ox * ux + oz * uz);
    float const side (std::abs(ox * uz - oz * ux));

    float const over_end (std::max(0.0f, std::max(-t, t - length)));
    float const over_side (std::max(0.0f, side - std::max(0.0f, width) * 0.5f));

    projection.along = std::clamp(t / length, 0.0f, 1.0f);
    projection.distance_to_core = std::sqrt(over_end * over_end + over_side * over_side);

    return projection;
}

bool World::buildRamp(glm::vec3 const& start, glm::vec3 const& end, float width, float falloff_width, bool smooth_falloff)
{
    ZoneScoped;

    // The grade is fixed by the two picks: the target height is linear in `along` over the whole
    // run, which is the property a sequence of flatten-at-angle strokes cannot hold and the reason
    // this tool is defined by two points rather than by a brush position.
    float const dx (end.x - start.x);
    float const dz (end.z - start.z);
    float const length (std::sqrt(dx * dx + dz * dz));

    // A thousandth of a world unit. Below that the direction of the ramp is numerical noise and
    // the projection along it is meaningless, so nothing is touched at all. The same threshold
    // guards projectOntoRamp above, and FlattenBlurTool checks it before opening an action.
    if (length < 0.001f)
    {
        LogError << "buildRamp: the two ramp points are " << length
                 << " yards apart horizontally; there is no direction to grade along." << std::endl;
        return false;
    }

    float const half_width (std::max(0.0f, width) * 0.5f);
    float const falloff (std::max(0.0f, falloff_width));

    // The search circle is centred on the midpoint and reaches the furthest corner of the falloff
    // rectangle: half the run, plus the half width, plus the band. for_all_chunks_in_range tests
    // a chunk's square against this circle (MapTile::chunks_in_range), so it hands back a superset
    // of the chunks the ramp touches -- never a subset, which is the direction that would matter.
    // The per-vertex test below is what actually decides, and a chunk where it finds nothing
    // returns false without ever being registered on the action.
    glm::vec3 const centre ((start + end) * 0.5f);
    float const search_radius (length * 0.5f + half_width + falloff);

    return for_all_chunks_in_range
      ( centre, search_radius
      , [&] (MapChunk* chunk)
        {
            // Value-initialised, not left indeterminate. `target` is only ever read where the
            // matching weight is positive and only ever written there too, but that correlation
            // is not something a compiler can see, and two 580-byte arrays (145 floats each)
            // zeroed once per chunk of a one-shot operation is not worth a suppressed warning.
            std::array<float, mapbufsize> weight{};
            std::array<float, mapbufsize> target{};
            bool any_affected (false);

            // Measured first, applied second, and that order is load-bearing:
            // registerChunkTerrainChange has to run before the first vertex of this chunk moves,
            // and it must not run at all for a chunk the ramp misses. A chunk registered but
            // unchanged costs 145 * 3 floats of pre-image plus the same again in post at
            // Action::finish (Action.cpp:437-445) -- 3.4 KB each, and a 200 yard diagonal ramp
            // has a search circle covering hundreds of chunks it never reaches.
            for (int i = 0; i < mapbufsize; ++i)
            {
                RampProjection const projection
                    (projectOntoRamp(start, end, width, chunk->mVertices[i]));

                weight[i] = rampFalloff(projection.distance_to_core, falloff, smooth_falloff);

                if (weight[i] > 0.0f)
                {
                    any_affected = true;
                    target[i] = start.y + (end.y - start.y) * projection.along;
                }
            }

            if (!any_affected)
            {
                return false;
            }

            NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);

            for (int i = 0; i < mapbufsize; ++i)
            {
                if (weight[i] > 0.0f)
                {
                    // Blend rather than assign, so the falloff band can be partial. In the core
                    // the weight is exactly 1 and this lands the vertex on the target.
                    chunk->mVertices[i].y += (target[i] - chunk->mVertices[i].y) * weight[i];
                }
            }

            // Vertices shared with the chunk next door are the same world position in both, so
            // both chunks compute the same weight and the same target for them and write the same
            // value. No gap fixing is needed for the same reason the other brushes need none.
            chunk->registerChunkUpdate(ChunkUpdateFlags::VERTEX);
            return true;
        }
      , [this] (MapChunk* chunk)
        {
            recalc_norms (chunk);
        }
      );
}

void World::blurTerrain(glm::vec3 const& pos, float remain, float radius, int BrushType, flatten_mode const& mode)
{
  ZoneScoped;
  for_all_chunks_in_range
    ( pos, radius
    , [&] (MapChunk* chunk)
      {
        NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
        return chunk->blurTerrain ( pos
                                  , remain
                                  , radius
                                  , BrushType
                                  , mode
                                  /*, [this](float x, float z) -> std::optional<float>
                                    {
                                      glm::vec3 vec;
                                      auto res (GetVertex (x, z, &vec));
                                      return res ? std::optional<float>(vec.y) : std::nullopt;
                                    }*/
                                  );
      }
    , [this] (MapChunk* chunk)
      {
        recalc_norms (chunk);
      }
    );
}

void World::recalc_norms (MapChunk* chunk) const
{
    ZoneScoped;
    chunk->recalcNorms();
}

bool World::paintTexture(glm::vec3 const& pos, Brush* brush, float strength, float pressure, scoped_blp_texture_reference texture)
{
  ZoneScoped;
  return for_all_chunks_in_range
    ( pos, brush->getRadius()
    , [&] (MapChunk* chunk)
      {
        NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
        return chunk->paintTexture(pos, brush, strength, pressure, texture);
      }
    );
}

bool World::stampTexture(glm::vec3 const& pos, Brush *brush, float strength, float pressure, scoped_blp_texture_reference texture, QImage* img, bool paint)
{
  ZoneScoped;
  return for_all_chunks_in_rect
    ( pos, brush->getRadius()
      , [&] (MapChunk* chunk)
      {
        NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
        return chunk->stampTexture(pos, brush, strength, pressure, texture, img, paint);
      }
    );
}

bool World::sprayTexture(glm::vec3 const& pos, Brush *brush, float strength, float pressure, float spraySize, float sprayPressure, scoped_blp_texture_reference texture)
{
  ZoneScoped;
  bool succ = false;
  float inc = brush->getRadius() / 4.0f;

  for (float pz = pos.z - spraySize; pz < pos.z + spraySize; pz += inc)
  {
    for (float px = pos.x - spraySize; px < pos.x + spraySize; px += inc)
    {
      if ((sqrt(pow(px - pos.x, 2) + pow(pz - pos.z, 2)) <= spraySize) && ((rand() % 1000) < sprayPressure))
      {
        succ |= paintTexture({px, pos.y, pz}, brush, strength, pressure, texture);
      }
    }
  }

  return succ;
}

bool World::replaceTexture(glm::vec3 const& pos, float radius, scoped_blp_texture_reference const& old_texture, scoped_blp_texture_reference new_texture, bool entire_chunk, bool entire_tile)
{
  ZoneScoped;

  if (entire_tile)
  {
      bool changed(false);

      for (MapTile* tile : mapIndex.tiles_in_range(pos, radius))
      {
          if (!tile->finishedLoading())
          {
              continue;
          }

          for (int i = 0; i < 16; ++i)
          {
              for (int j = 0; j < 16; ++j)
              {
                  MapChunk* chunk = tile->getChunk(i, j);
                  NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
                  if (chunk->replaceTexture(pos, radius, old_texture, new_texture, true))
                  {
                      changed = true;
                      mapIndex.setChanged(tile);
                  }
              }
          }
      }
      return changed;
  }
  else
  {
    return for_all_chunks_in_range
      ( pos, radius
        , [&](MapChunk* chunk)
        {
          NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
          return chunk->replaceTexture(pos, radius, old_texture, new_texture, entire_chunk);
        }
      );
  }
}

void World::eraseTextures(glm::vec3 const& pos)
{
  ZoneScoped;
  for_chunk_at(pos, [](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
    chunk->eraseTextures();
  });
}

void World::overwriteTextureAtCurrentChunk(glm::vec3 const& pos, scoped_blp_texture_reference const& oldTexture, scoped_blp_texture_reference newTexture)
{
  ZoneScoped;
  for_chunk_at(pos, [&](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
    chunk->switchTexture(oldTexture, std::move (newTexture));
  });
}

void World::paintGroundEffectExclusion(glm::vec3 const& pos, float radius, bool exclusion)
{
    ZoneScoped;
    for_all_chunks_in_range
    (pos, radius
        , [&](MapChunk* chunk)
        {
            // TODO action
            NOGGIT_CUR_ACTION->registerChunkDetailDoodadExclusionChange(chunk);

            // chunk->setHole(pos, radius, exclusion);
            chunk->paintDetailDoodadsExclusion(pos, radius, exclusion);
            return true;
        }
    );
}

void World::setHole(glm::vec3 const& pos, float radius, bool big, bool hole)
{
  ZoneScoped;
  for_all_chunks_in_range
      ( pos, radius
        , [&](MapChunk* chunk)
        {
          NOGGIT_CUR_ACTION->registerChunkHoleChange(chunk);
          chunk->setHole(pos, radius, big, hole);
          return true;
        }
      );
}

void World::setHoleADT(glm::vec3 const& pos, bool hole)
{
  ZoneScoped;

  for_all_chunks_on_tile(pos, [&](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkHoleChange(chunk);
    chunk->setHole(pos, 1.0f, true, hole);
  });
}

void World::loadAllTiles(glm::vec3& camera_pos)
{
  ZoneScoped;

  // for (size_t z = 0; z < 64; z++)
  // {
  //   for (size_t x = 0; x < 64; x++)
  //   {
  //     TileIndex tile(x, z);
  // 
  //     MapTile* mTile = mapIndex.loadTile(tile);
  // 
  //     if (mTile)
  //     {
  //       // mTile->wait_until_loaded();
  //     }
  //   }
  // }

  // test loading tiles from player to outer
  // Create a vector to hold distances and corresponding tiles
  std::vector<std::pair<float, TileIndex>> distanceTilePairs;
  // Fill the vector with Manhattan distances and tiles
  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      TileIndex tile(x, z);

      if (!mapIndex.hasTile(tile))
        continue;

      // int distance = calculateManhattanDistance(playerRow, playerCol, tile);
      float playerRow = /*std::floor*/(camera_pos.x / TILESIZE);
      float playerCol = /*std::floor*/(camera_pos.z / TILESIZE);
      float distance = std::abs(playerRow - tile.x) + std::abs(playerCol - tile.z);
      // tile.dist()
      distanceTilePairs.emplace_back(distance, tile);

    }
  }
  // Sort the vector based on distance
  std::sort(distanceTilePairs.begin(), distanceTilePairs.end(),
    [](const auto& a, const auto& b) {
      return a.first < b.first; // Sort by distance
    });

  for (const auto& pair : distanceTilePairs)
  {
    MapTile* mTile = mapIndex.loadTile(pair.second);

    if (mTile)
    {
      // mTile->wait_until_loaded();
    }
  }
}

unsigned World::getNumLoadedTiles() const
{
  return _n_loaded_tiles;
}

unsigned World::getNumRenderedTiles() const
{
  return _n_rendered_tiles;
}

unsigned World::getNumRenderedObjects() const
{
  return _n_rendered_objects;
}

void World::convert_alphamap(QProgressDialog* progress_dialog, bool to_big_alpha)
{
  ZoneScoped;

  if (to_big_alpha == mapIndex.hasBigAlpha())
  {
    return;
  }

  //! How often the progress dialog is advanced, in tiles. See the setValue call in the loop.
  constexpr int PROGRESS_PUMP_TILE_INTERVAL = 16;

  int count = 0;
  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      // not cancellable.
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);

      if (mTile)
      {
        mTile->wait_until_loaded();

        mTile->convert_alphamap(to_big_alpha);
        mTile->saveTile(this);
        mapIndex.markOnDisc (tile, true);
        mapIndex.unsetChanged(tile);

        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
        count++;

        // Every 16th tile, not every tile, and the final value set once after the loop.
        //
        // QProgressDialog::setValue calls QCoreApplication::processEvents() on a modal dialog, so
        // each of these is a full trip through the event loop -- and a trip through the event loop
        // here can dispatch a queued repaint of the 3D view. At one call per tile that is up to
        // 4096 event-loop re-entries, and potentially 4096 rendered frames, interleaved with a
        // bulk save. Throttling is the pattern NoggitWindow.cpp:1023-1027 already uses and
        // documents for the same reason.
        //
        // The trailing setValue is load-bearing, not decoration: QProgressDialog closes itself
        // when its value reaches its maximum, so a throttle that could skip the last update would
        // leave the dialog on screen. This loop is not cancellable (see the note above) and holds
        // no pointer across the pump, so nothing else here depends on the interval.
        if (count % PROGRESS_PUMP_TILE_INTERVAL == 0)
        {
          progress_dialog->setValue(count);
        }
      }
    }
  }

  // Guarded: when count is an exact multiple of the interval, the in-loop call above already
  // delivered this exact value, and QProgressDialog with autoReset treats reaching the maximum as
  // "done" -- it resets and hides. Calling setValue again with the same value re-shows and
  // re-hides the dialog, a visible flash at the end of a long conversion.
  if (count % PROGRESS_PUMP_TILE_INTERVAL != 0)
  {
    progress_dialog->setValue(count);
  }

  mapIndex.convert_alphamap(to_big_alpha);
  mapIndex.save();
}


void World::deleteModelInstance(int uid, bool action)
{
  ZoneScoped;
  auto instance = _model_instance_storage.get_model_instance(uid);

  if (instance)
  {
    _model_instance_storage.delete_instance(uid, action);
    need_model_updates = true;
    reset_selection();
  }
}

void World::deleteWMOInstance(int uid, bool action)
{
  ZoneScoped;
  auto instance = _model_instance_storage.get_wmo_instance(uid);

  if (instance)
  {
    _model_instance_storage.delete_instance(uid, action);
    need_model_updates = true;
    reset_selection();
  }
}

void World::deleteInstance(int uid, bool action)
{
  ZoneScoped;
  auto instance = _model_instance_storage.get_instance(uid);

  if (instance)
  {
    _model_instance_storage.delete_instance(uid, action);
    need_model_updates = true;
    reset_selection();
  }
}

bool World::uid_duplicates_found() const
{
  ZoneScoped;
  return _model_instance_storage.uid_duplicates_found();
}

void World::delete_duplicate_model_and_wmo_instances()
{
  ZoneScoped;
  reset_selection();

  _model_instance_storage.clear_duplicates(false);
  need_model_updates = true;
}

void World::unload_every_model_and_wmo_instance()
{
  ZoneScoped;
  reset_selection();

  _model_instance_storage.clear();

  // _models_by_filename.clear();
}

void World::addM2 ( BlizzardArchive::Listfile::FileKey const& file_key
                  , glm::vec3 newPos
                  , float scale
                  , glm::vec3 rotation
                  , Noggit::object_paste_params* paste_params
                  , bool action
                  )
{
  ZoneScoped;
  ModelInstance model_instance = ModelInstance(file_key, _context);

  model_instance.uid = mapIndex.newGUID();
  model_instance.pos = newPos;
  model_instance.scale = scale;
  model_instance.dir = rotation;

  if (paste_params)
  {
    if (_settings->value("model/random_rotation", false).toBool())
    {
      float min = paste_params->minRotation;
      float max = paste_params->maxRotation;
      model_instance.dir.y += math::degrees(misc::randfloat(min, max))._;
    }

    if (_settings->value ("model/random_tilt", false).toBool ())
    {
      float min = paste_params->minTilt;
      float max = paste_params->maxTilt;
      model_instance.dir.x += math::degrees(misc::randfloat(min, max))._;
      model_instance.dir.z += math::degrees(misc::randfloat(min, max))._;
    }

    if (_settings->value ("model/random_size", false).toBool ())
    {
      float min = paste_params->minScale;
      float max = paste_params->maxScale;
      model_instance.scale = misc::randfloat(min, max);
    }
  }

  // to ensure the tiles are updated correctly
  model_instance.model->wait_until_loaded();
  model_instance.recalcExtents();

  std::uint32_t uid = _model_instance_storage.add_model_instance(std::move(model_instance), true, action);

  // _models_by_filename[file_key.filepath()].push_back(_model_instance_storage.get_model_instance(uid).value());
}

ModelInstance* World::addM2AndGetInstance ( BlizzardArchive::Listfile::FileKey const& file_key
    , glm::vec3 newPos
    , float scale
    , math::degrees::vec3 rotation
    , Noggit::object_paste_params* paste_params
    , bool ignore_params
    , bool action
)
{
  ZoneScoped;
  ModelInstance model_instance = ModelInstance(file_key, _context);

  model_instance.uid = mapIndex.newGUID();
  model_instance.pos = newPos;
  model_instance.scale = scale;
  model_instance.dir = rotation;

  if (paste_params && !ignore_params)
  {
    if (_settings->value("model/random_rotation", false).toBool())
    {
      float min = paste_params->minRotation;
      float max = paste_params->maxRotation;
      model_instance.dir.y += math::degrees(misc::randfloat(min, max))._;
    }

    if (_settings->value ("model/random_tilt", false).toBool ())
    {
      float min = paste_params->minTilt;
      float max = paste_params->maxTilt;
      model_instance.dir.x += math::degrees(misc::randfloat(min, max))._;
      model_instance.dir.z += math::degrees(misc::randfloat(min, max))._;
    }

    if (_settings->value ("model/random_size", false).toBool ())
    {
      float min = paste_params->minScale;
      float max = paste_params->maxScale;
      model_instance.scale = misc::randfloat(min, max);
    }
  }

  // to ensure the tiles are updated correctly
  model_instance.model->wait_until_loaded();
  model_instance.recalcExtents();

  std::uint32_t uid = _model_instance_storage.add_model_instance(std::move(model_instance), true, action);

  auto instance = _model_instance_storage.get_model_instance(uid); // .value();
  // _models_by_filename[file_key.filepath()].push_back(instance);

  return instance.value();
}

void World::addWMO ( BlizzardArchive::Listfile::FileKey const& file_key
                   , glm::vec3 newPos
                   , float scale
                   , math::degrees::vec3 rotation
                   , Noggit::object_paste_params* paste_params
                   , bool action
                   )
{
  ZoneScoped;
  WMOInstance wmo_instance(file_key, _context);

  wmo_instance.uid = mapIndex.newGUID();
  wmo_instance.pos = newPos;
  wmo_instance.dir = rotation;

  if (paste_params)
  {
      if (_settings->value("model/random_rotation", false).toBool())
      {
          float min = paste_params->minRotation;
          float max = paste_params->maxRotation;
          wmo_instance.dir.y += math::degrees(misc::randfloat(min, max))._;
      }

      if (_settings->value("model/random_tilt", false).toBool())
      {
          float min = paste_params->minTilt;
          float max = paste_params->maxTilt;
          wmo_instance.dir.x += math::degrees(misc::randfloat(min, max))._;
          wmo_instance.dir.z += math::degrees(misc::randfloat(min, max))._;
      }

      if (_settings->value("model/random_size", false).toBool())
      {
          float min = paste_params->minScale;
          float max = paste_params->maxScale;
          wmo_instance.scale = misc::randfloat(min, max);
      }
  }


  // to ensure the tiles are updated correctly
  wmo_instance.wmo->wait_until_loaded();
  wmo_instance.recalcExtents();

  _model_instance_storage.add_wmo_instance(std::move(wmo_instance), true, action);
}

WMOInstance* World::addWMOAndGetInstance ( BlizzardArchive::Listfile::FileKey const& file_key
    , glm::vec3 newPos
    , math::degrees::vec3 rotation
    , float scale
    , bool action
)
{
  ZoneScoped;
  WMOInstance wmo_instance(file_key, _context);

  wmo_instance.uid = mapIndex.newGUID();
  wmo_instance.pos = newPos;
  wmo_instance.dir = rotation;
  wmo_instance.scale = scale;

  // to ensure the tiles are updated correctly
  wmo_instance.wmo->wait_until_loaded();
  wmo_instance.recalcExtents();

  std::uint32_t uid = _model_instance_storage.add_wmo_instance(std::move(wmo_instance), true, action);

  auto instance = _model_instance_storage.get_wmo_instance(uid);

  return instance.value();
}


std::uint32_t World::add_model_instance(ModelInstance model_instance, bool from_reloading, bool action)
{
  ZoneScoped;
  return _model_instance_storage.add_model_instance(std::move(model_instance), from_reloading, action);
}

std::uint32_t World::add_wmo_instance(WMOInstance wmo_instance, bool from_reloading, bool action)
{
  ZoneScoped;
  return _model_instance_storage.add_wmo_instance(std::move(wmo_instance), from_reloading, action);
}

std::optional<selection_type> World::get_model(std::uint32_t uid)
{
  ZoneScoped;
  return _model_instance_storage.get_instance(uid);
}

void World::remove_models_if_needed(std::vector<uint32_t> const& uids)
{
  ZoneScoped;
  // todo: manage instances properly
  // don't unload anything during the uid fix all,
  // otherwise models spanning several adts will be unloaded too soon
  if (mapIndex.uid_fix_all_in_progress())
  {
    return;
  }

  for (uint32_t uid : uids)
  {
    // it handles the removal from the selection if necessary
    _model_instance_storage.unload_instance_and_remove_from_selection_if_necessary(uid);
  }

  // deselect the terrain when an adt is unloaded
  if (_current_selection.size() == 1 && _current_selection.at(0).index() == eEntry_MapChunk)
  {
    reset_selection();
  }
  else
  {
    update_selection_pivot();
  }
  /*
  if (uids.size())
  {
    need_model_updates = true;
  }*/
}

void World::reload_tile(TileIndex const& tile)
{
  ZoneScoped;
  reset_selection();
  mapIndex.reloadTile(tile);
}

void World::deleteObjects(std::vector<selected_object_type> const& types, bool action)
{
  ZoneScoped;
  _model_instance_storage.delete_instances(types, action);
  need_model_updates = true;
}

void World::updateTilesEntry(selection_type const& entry, model_update type,
                             tile_dirty_intent intent)
{
  ZoneScoped;
  if (entry.index() != eEntry_Object)
    return;

  auto obj = std::get<selected_object_type>(entry);

  if (obj->which() == eWMO)
    updateTilesWMO (static_cast<WMOInstance*>(obj), type, intent);
  else if (obj->which() == eMODEL)
    updateTilesModel (static_cast<ModelInstance*>(obj), type, intent);

}


void World::updateTilesEntry(SceneObject* entry, model_update type, tile_dirty_intent intent)
{
  ZoneScoped;
  if (entry->which() == eWMO)
    updateTilesWMO (static_cast<WMOInstance*>(entry), type, intent);
  else if (entry->which() == eMODEL)
    updateTilesModel (static_cast<ModelInstance*>(entry), type, intent);

}

void World::updateTilesWMO(WMOInstance* wmo, model_update type, tile_dirty_intent intent)
{
  ZoneScoped;
  _tile_update_queue.queue_update(wmo, type, intent);
}

void World::updateTilesModel(ModelInstance* m2, model_update type, tile_dirty_intent intent)
{
  ZoneScoped;
  _tile_update_queue.queue_update(m2, type, intent);
}

void World::wait_for_all_tile_updates()
{
  ZoneScoped;
  _tile_update_queue.wait_for_all_update();
}

unsigned int World::getMapID() const
{
  ZoneScoped;
  return mapIndex._map_id;
}

void World::clearTextures(glm::vec3 const& pos)
{
  ZoneScoped;
  for_all_chunks_on_tile(pos, [](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
    chunk->eraseTextures();
  });
}


void World::exportADTAlphamap(glm::vec3 const& pos)
{
  ZoneScoped;
  for_tile_at ( pos
    , [&] (MapTile* tile)
    {
      QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
      if (!(path.endsWith('\\') || path.endsWith('/')))
      {
        path += "/";
      }

      QDir dir(path + "/world/maps/" + basename.c_str());
      if (!dir.exists())
        dir.mkpath(".");

      for (int i = 1; i < 4; ++i)
      {
        QImage img = tile->getAlphamapImage(i);
        img.save(path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
        + "_" + std::to_string(tile->index.x).c_str() + "_" + std::to_string(tile->index.z).c_str()
        + "_layer" + std::to_string(i).c_str() + ".png", "PNG");
      }

    }
  );
}

void World::exportADTNormalmap(glm::vec3 const& pos)
{
  ZoneScoped;
  for_tile_at ( pos
    , [&] (MapTile* tile)
      {
        QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
        if (!(path.endsWith('\\') || path.endsWith('/')))
        {
          path += "/";
        }

        QDir dir(path + "/world/maps/" + basename.c_str());
        if (!dir.exists())
          dir.mkpath(".");

        QImage img = tile->getNormalmapImage();
        img.save(path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                 + "_" + std::to_string(tile->index.x).c_str() + "_" + std::to_string(tile->index.z).c_str()
                 + "_normal.png", "PNG");
      }
  );
}

void World::exportADTAlphamap(glm::vec3 const& pos, std::string const& filename)
{
  ZoneScoped;
  for_tile_at ( pos
    , [&] (MapTile* tile)
    {
      QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
      if (!(path.endsWith('\\') || path.endsWith('/')))
      {
        path += "/";
      }

      QDir dir(path + "/world/maps/" + basename.c_str());
      if (!dir.exists())
        dir.mkpath(".");

      QString tex(filename.c_str());
      QImage img = tile->getAlphamapImage(filename);
      img.save(path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
               + "_" + std::to_string(tile->index.x).c_str() + "_" + std::to_string(tile->index.z).c_str()
               + "_" + tex.replace("/", "-") + ".png", "PNG");

    }
  );
}

void World::exportADTHeightmap(glm::vec3 const& pos, float min_height, float max_height)
{
  ZoneScoped;
  for_tile_at ( pos
    , [&] (MapTile* tile)
                {
                  QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
                  if (!(path.endsWith('\\') || path.endsWith('/')))
                  {
                    path += "/";
                  }

                  QDir dir(path + "/world/maps/" + basename.c_str());
                  if (!dir.exists())
                    dir.mkpath(".");

                  QImage img = tile->getHeightmapImage(min_height, max_height);
                  img.save(path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                           + "_" + std::to_string(tile->index.x).c_str() + "_" + std::to_string(tile->index.z).c_str()
                           + "_height.png", "PNG");


                }
  );
}

void World::exportADTVertexColorMap(glm::vec3 const& pos)
{
  ZoneScoped;
  for_tile_at ( pos
    , [&] (MapTile* tile)
                {
                  QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
                  if (!(path.endsWith('\\') || path.endsWith('/')))
                  {
                    path += "/";
                  }

                  QDir dir(path + "/world/maps/" + basename.c_str());
                  if (!dir.exists())
                    dir.mkpath(".");

                  QImage img = tile->getVertexColorsImage();
                  img.save(path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                           + "_" + std::to_string(tile->index.x).c_str() + "_" + std::to_string(tile->index.z).c_str()
                           + "_vcol.png", "PNG");


                }
  );
}

void World::importADTAlphamap(glm::vec3 const& pos, QImage const& image, unsigned layer, bool cleanup)
{
  ZoneScoped;
  for_all_chunks_on_tile(pos, [](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
  });

  if (image.width() != 1024 || image.height() != 1024)
  {
    QImage scaled = image.scaled(1024, 1024, Qt::AspectRatioMode::IgnoreAspectRatio);

    for_tile_at ( pos
      , [&] (MapTile* tile)
                  {
                    tile->setAlphaImage(scaled, layer, cleanup);
                  }
    );

  }
  else
  {
    for_tile_at ( pos
      , [&] (MapTile* tile)
      {
        tile->setAlphaImage(image, layer, cleanup);
      }
    );
  }

}

void World::importADTAlphamap(glm::vec3 const& pos, bool cleanup)
{
  ZoneScoped;
  for_all_chunks_on_tile(pos, [](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
  });

  QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
  if (!(path.endsWith('\\') || path.endsWith('/')))
  {
    path += "/";
  }

  for_tile_at ( pos
    , [&] (MapTile* tile)
    {
      for (int i = 1; i < 4; ++i)
      {
        QString filename = path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                       + "_" + std::to_string(tile->index.x).c_str() + "_" + std::to_string(tile->index.z).c_str()
                       + "_layer" +  std::to_string(i).c_str() + ".png";

        if(!QFileInfo::exists(filename))
          continue;

        QImage img;
        img.load(filename, "PNG");

        if (img.width() != 1024 || img.height() != 1024)
          img = img.scaled(1024, 1024, Qt::AspectRatioMode::IgnoreAspectRatio);

        tile->setAlphaImage(img, i, true);
      }

    }
  );
}

void World::importADTHeightmap(glm::vec3 const& pos, QImage const& image, float min_height, float max_height, unsigned mode, bool tiledEdges)
{
  ZoneScoped;
  int desired_dimensions = tiledEdges ? 256 : 257;
  for_all_chunks_on_tile(pos, [](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
  });

  if (image.width() != desired_dimensions || image.height() != desired_dimensions)
  {
    QImage scaled = image.scaled(desired_dimensions, desired_dimensions, Qt::AspectRatioMode::IgnoreAspectRatio);

    for_tile_at ( pos
      , [&] (MapTile* tile)
      {
        tile->setHeightmapImage(scaled, min_height, max_height, mode, tiledEdges);
      }
    );

  }
  else
  {
    for_tile_at ( pos
      , [&] (MapTile* tile)
      {
        tile->setHeightmapImage(image, min_height, max_height, mode, tiledEdges);
      }
    );
  }
}

void World::importADTHeightmap(glm::vec3 const& pos, float min_height, float max_height, unsigned mode, bool tiledEdges)
{
  ZoneScoped;
  for_tile_at ( pos
    , [&] (MapTile* tile)
    {

      QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
      if (!(path.endsWith('\\') || path.endsWith('/')))
      {
        path += "/";
      }

      QString filename = path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                         + "_" + std::to_string(tile->index.x).c_str() + "_" + std::to_string(tile->index.z).c_str()
                         + "_height" + ".png";

      if (!QFileInfo::exists(filename))
      {
          QMessageBox::warning
          (nullptr
              , "File not found"
              , "File not found: " + filename
              , QMessageBox::Ok
          );
        return;
      }


      for_all_chunks_on_tile(pos, [](MapChunk* chunk)
      {
        NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
      });

      QImage img;
      img.load(filename, "PNG");

      size_t desiredSize = tiledEdges ? 256 : 257;
      if (img.width() != desiredSize || img.height() != desiredSize)
        img = img.scaled(static_cast<int>(desiredSize), static_cast<int>(desiredSize), Qt::AspectRatioMode::IgnoreAspectRatio);

      tile->setHeightmapImage(img, min_height, max_height, mode, tiledEdges);

    }
  );
}

void World::importADTWatermap(glm::vec3 const& pos, QImage const& image, float min_height, float max_height, unsigned mode, bool tiledEdges)
{
    ZoneScoped;
    int desired_dimensions = tiledEdges ? 256 : 257;
    for_all_chunks_on_tile(pos, [](MapChunk* chunk)
        {
            NOGGIT_CUR_ACTION->registerChunkLiquidChange(chunk);
        });

    if (image.width() != desired_dimensions || image.height() != desired_dimensions)
    {
        QImage scaled = image.scaled(desired_dimensions, desired_dimensions, Qt::AspectRatioMode::IgnoreAspectRatio);

        for_tile_at(pos
            , [&](MapTile* tile)
            {
                tile->Water.setWatermapImage(scaled, min_height, max_height, mode, tiledEdges);
            }
        );

    }
    else
    {
        for_tile_at(pos
            , [&](MapTile* tile)
            {
                tile->Water.setWatermapImage(image, min_height, max_height, mode, tiledEdges);
            }
        );
    }
}

void World::importADTVertexColorMap(glm::vec3 const& pos, int mode, bool tiledEdges)
{
  ZoneScoped;
  for_tile_at ( pos
    , [&] (MapTile* tile)
      {

        QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
        if (!(path.endsWith('\\') || path.endsWith('/')))
        {
          path += "/";
        }

        QString filename = path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                           + "_" + std::to_string(tile->index.x).c_str() + "_" + std::to_string(tile->index.z).c_str()
                           + "_vcol" + ".png";

        if(!QFileInfo::exists(filename))
          return;

        for_all_chunks_on_tile(pos, [](MapChunk* chunk)
        {
          NOGGIT_CUR_ACTION->registerChunkVertexColorChange(chunk);
        });

        QImage img;
        img.load(filename, "PNG");

        size_t desiredSize = tiledEdges ? 256 : 257;
        if (img.width() != desiredSize || img.height() != desiredSize)
          img = img.scaled(static_cast<int>(desiredSize), static_cast<int>(desiredSize), Qt::AspectRatioMode::IgnoreAspectRatio);

        tile->setVertexColorImage(img, mode, tiledEdges);

      }
  );
}

void World::ensureAllTilesetsADT(glm::vec3 const& pos)
{
  ZoneScoped;
  static QStringList textures {"tileset/generic/black.blp",
                               "tileset/generic/red.blp",
                               "tileset/generic/green.blp",
                               "tileset/generic/blue.blp",};

  for_all_chunks_on_tile(pos, [=](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);

    for (int i = 0; i < 4; ++i)
    {
      if (chunk->texture_set->num() <= i)
      {
        scoped_blp_texture_reference tex {textures[i].toStdString(), Noggit::NoggitRenderContext::MAP_VIEW};
        chunk->texture_set->addTexture(tex);
      }
    }

  });
}

void World::importADTVertexColorMap(glm::vec3 const& pos, QImage const& image, int mode, bool tiledEdges)
{
  ZoneScoped;
  for_all_chunks_on_tile(pos, [](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkVertexColorChange(chunk);
  });

  size_t desiredDimensions = tiledEdges ? 256 : 257;

  if (image.width() != desiredDimensions || image.height() != desiredDimensions)
  {
    QImage scaled = image.scaled(static_cast<int>(desiredDimensions), static_cast<int>(desiredDimensions), Qt::AspectRatioMode::IgnoreAspectRatio);

    for_tile_at ( pos
      , [&] (MapTile* tile)
        {
          tile->setVertexColorImage(scaled, mode, tiledEdges);
        }
    );

  }
  else
  {
    for_tile_at ( pos
      , [&] (MapTile* tile)
        {
          tile->setVertexColorImage(image, mode, tiledEdges);
        }
    );
  }
}

void World::setBaseTexture(glm::vec3 const& pos)
{
  ZoneScoped;
  for_all_chunks_on_tile(pos, [](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
    chunk->eraseTextures();
    if (!!Noggit::Ui::selected_texture::get())
    {
      chunk->addTexture(*Noggit::Ui::selected_texture::get());
    }
  });
}

void World::clear_shadows(glm::vec3 const& pos)
{
  ZoneScoped;
  for_all_chunks_on_tile(pos, [] (MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkShadowChange(chunk);
    chunk->clear_shadows();
  });
}

void World::swapTexture(glm::vec3 const& pos, scoped_blp_texture_reference tex)
{
  ZoneScoped;
  if (!!Noggit::Ui::selected_texture::get())
  {
    for_all_chunks_on_tile(pos, [&](MapChunk* chunk)
    {
      NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
      chunk->switchTexture(tex, *Noggit::Ui::selected_texture::get());
    });
  }
}

void World::swapTextureGlobal(scoped_blp_texture_reference tex)
{
    ZoneScoped;
    if (!!Noggit::Ui::selected_texture::get())
    {

        for (size_t z = 0; z < 64; z++)
        {
            for (size_t x = 0; x < 64; x++)
            {
                TileIndex tile(x, z);

                bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
                MapTile* mTile = mapIndex.loadTile(tile);

                if (mTile)
                {
                    mTile->wait_until_loaded();

                    bool tile_changed = false;
                    for_all_chunks_on_tile(mTile, [&](MapChunk* chunk)
                    {
                        // NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
                        bool swapped = chunk->switchTexture(tex, *Noggit::Ui::selected_texture::get());
                        if (swapped)
                            tile_changed = true;
                    });

                    if (tile_changed)
                    {
                        mTile->saveTile(this);
                        mapIndex.markOnDisc(tile, true);
                        mapIndex.unsetChanged(tile);
                    }

                    if (unload)
                    {
                        mapIndex.unloadTile(tile);
                    }
                }
            }
        }
    }
}

void World::removeTexture(glm::vec3 const& pos, scoped_blp_texture_reference tex)
{
    ZoneScoped;
    if (!!Noggit::Ui::selected_texture::get())
    {
        for_all_chunks_on_tile(pos, [&](MapChunk* chunk)
            {
                NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
                // chunk->switchTexture(tex, *Noggit::Ui::selected_texture::get());
                chunk->eraseTexture(tex);
            });
    }
}


void World::removeTexDuplicateOnADT(glm::vec3 const& pos)
{
  ZoneScoped;
  for_all_chunks_on_tile(pos, [](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
    chunk->texture_set->removeDuplicate();
  } );
}

void World::change_texture_flag(glm::vec3 const& pos, scoped_blp_texture_reference const& tex, std::size_t flag, bool add)
{
  ZoneScoped;
  for_chunk_at(pos, [&] (MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
    chunk->change_texture_flag(tex, flag, add);
  });
}

void World::paintLiquid( glm::vec3 const& pos
                       , float radius
                       , int liquid_id
                       , bool add
                       , math::radians const& angle
                       , math::radians const& orientation
                       , bool lock
                       , glm::vec3 const& origin
                       , bool override_height
                       , bool override_liquid_id
                       , float opacity_factor
                       )
{
  ZoneScoped;
  for_all_chunks_in_range(pos, radius, [&](MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkLiquidChange(chunk);
    chunk->liquid_chunk()->paintLiquid(pos, radius, liquid_id, add, angle, orientation, lock, origin, override_height, override_liquid_id, chunk, opacity_factor);
    return true;
  });
}

bool World::getLiquidHeight (float x, float z, int liquid_id, float& height)
{
  // getChunkAt already returns nullptr for an unloaded or still-loading tile, which is the
  // answer the smooth kernel wants at a map edge: that tap simply does not contribute.
  MapChunk* chunk (getChunkAt (glm::vec3 (x, 0.f, z)));

  if (!chunk)
  {
    return false;
  }

  ChunkWater* water (chunk->liquid_chunk());

  return water && water->liquidHeightAt (x, z, liquid_id, height);
}

void World::changeLiquidHeight ( glm::vec3 const& pos
                               , float change
                               , float radius
                               , float inner_radius
                               , int brush_type
                               , float opacity_factor
                               )
{
  ZoneScoped;
  for_all_chunks_in_range (pos, radius, [&] (MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkLiquidChange (chunk);
    return chunk->liquid_chunk()->changeLiquidHeight (pos, change, radius, inner_radius, brush_type
                                                     , chunk, opacity_factor);
  });
}

void World::flattenLiquidHeight ( glm::vec3 const& pos
                                , float remain
                                , float radius
                                , int brush_type
                                , flatten_mode const& mode
                                , glm::vec3 const& origin
                                , math::degrees angle
                                , math::degrees orientation
                                , float opacity_factor
                                )
{
  ZoneScoped;
  for_all_chunks_in_range (pos, radius, [&] (MapChunk* chunk)
  {
    NOGGIT_CUR_ACTION->registerChunkLiquidChange (chunk);
    return chunk->liquid_chunk()->flattenLiquidHeight (pos, remain, radius, brush_type, mode, origin
                                                      , math::radians (angle), math::radians (orientation)
                                                      , chunk, opacity_factor);
  });
}

void World::smoothLiquidHeight ( glm::vec3 const& pos
                               , float remain
                               , float radius
                               , int brush_type
                               , flatten_mode const& mode
                               , float opacity_factor
                               )
{
  ZoneScoped;

  // Two passes, and the reason is the seam. The smooth kernel reaches up to four grid cells
  // (16.67 yards) past the vertex it is smoothing, so for a vertex on a chunk border the taps
  // land in the neighbouring chunk. If the stroke wrote chunk N before gathering for chunk N+1,
  // the second chunk would average against heights the first pass had already moved, the two
  // owners of the shared vertex would compute different targets, and the tool would manufacture
  // the tear it is meant to remove. Gathering everything first makes the whole stroke read one
  // consistent pre-stroke state, which is also what makes the result independent of chunk
  // iteration order.
  std::unordered_map<MapChunk*, std::vector<LiquidHeightPatch>> patches;

  std::function<bool (float, float, int, float&)> const sampler
    ( [this] (float x, float z, int liquid_id, float& height)
      {
        return getLiquidHeight (x, z, liquid_id, height);
      }
    );

  for_all_chunks_in_range (pos, radius, [&] (MapChunk* chunk)
  {
    std::vector<LiquidHeightPatch> chunk_patches;

    if (chunk->liquid_chunk()->gatherSmoothedLiquidHeights (pos, remain, radius, brush_type, mode
                                                           , sampler, chunk_patches))
    {
      patches.emplace (chunk, std::move (chunk_patches));
    }

    // Nothing was written, so this chunk's tile is not dirty yet. The apply pass below reports
    // the change and marks it.
    return false;
  });

  if (patches.empty())
  {
    return;
  }

  // The second range query returns the same chunk set as the first - same centre, same radius,
  // same predicate - so every gathered patch is applied.
  for_all_chunks_in_range (pos, radius, [&] (MapChunk* chunk)
  {
    auto const found (patches.find (chunk));

    if (found == patches.end())
    {
      return false;
    }

    NOGGIT_CUR_ACTION->registerChunkLiquidChange (chunk);
    return chunk->liquid_chunk()->applyLiquidHeightPatches (found->second, chunk, opacity_factor);
  });
}

void World::weldLiquidSeams (glm::vec3 const& pos, float radius, float opacity_factor)
{
  ZoneScoped;

  // A liquid vertex on a chunk border is stored once per owning chunk: twice on an edge, four
  // times at a corner. The brushes above keep every copy in step, but a map can arrive with the
  // copies already disagreeing, and that pre-existing gap is what the user sees as a tear
  // between two water heights. This forces them all to their mean.
  //
  // Only the 32 border vertices of the 9x9 grid can be shared (x or z equal to 0 or 8); the
  // other 49 are interior and are skipped. Each shared vertex is reached from up to four
  // chunks, so it is processed more than once per pass - which is harmless, because once the
  // copies are equal their mean is that same value and a second visit writes nothing new.
  //
  // Cost, for the record: a chunk box is 33.33 yards, so a 40-yard brush reaches at most a 5x5
  // block, 25 chunks. 25 x 32 x 4 probes is 3200 getChunkAt calls, each an index lookup. The
  // part that would NOT have been cheap is the derived state - min/max, the fishable and
  // fatigue masks, the render tag - so the writes leave it stale and one
  // finishLiquidHeightEdit per touched chunk brings it back, rather than one per vertex.
  float const half_unit (UNITSIZE * 0.5f);

  std::unordered_set<ChunkWater*> touched;

  for_all_chunks_in_range (pos, radius, [&] (MapChunk* chunk)
  {
    ChunkWater* water (chunk->liquid_chunk());

    if (!water)
    {
      return false;
    }

    bool changed (false);

    for (liquid_layer& layer : *water->getLayers())
    {
      int const liquid_id (layer.liquidID());
      int const origin_x (layer.gridOriginX());
      int const origin_z (layer.gridOriginZ());

      for (int z (0); z < 9; ++z)
      {
        for (int x (0); x < 9; ++x)
        {
          if (x != 0 && x != 8 && z != 0 && z != 8)
          {
            continue;
          }

          float height;

          if (!layer.vertexHeight (x, z, height))
          {
            continue;
          }

          float const vertex_x (liquid_layer::gridCoord (origin_x + x));
          float const vertex_z (liquid_layer::gridCoord (origin_z + z));

          if (misc::dist (vertex_x, vertex_z, pos.x, pos.z) > radius)
          {
            continue;
          }

          // Probe the centre of each of the four grid cells meeting at this vertex. Each probe
          // lands unambiguously inside one chunk, and the set of distinct chunks it finds is
          // exactly the set of owners of this vertex.
          MapChunk* owners[4] = {nullptr, nullptr, nullptr, nullptr};
          int owner_count (0);

          for (int quadrant (0); quadrant < 4; ++quadrant)
          {
            float const probe_x (vertex_x + ((quadrant & 1) ? half_unit : -half_unit));
            float const probe_z (vertex_z + ((quadrant & 2) ? half_unit : -half_unit));

            MapChunk* owner (getChunkAt (glm::vec3 (probe_x, 0.f, probe_z)));

            if (!owner || !owner->liquid_chunk())
            {
              continue;
            }

            bool duplicate (false);

            for (int i (0); i < owner_count; ++i)
            {
              duplicate |= owners[i] == owner;
            }

            if (!duplicate)
            {
              owners[owner_count++] = owner;
            }
          }

          float total (0.f);
          int samples (0);

          for (int i (0); i < owner_count; ++i)
          {
            float owner_height;

            if (owners[i]->liquid_chunk()->liquidHeightAt (vertex_x, vertex_z, liquid_id, owner_height))
            {
              total += owner_height;
              ++samples;
            }
          }

          if (samples < 2)
          {
            continue;
          }

          float const mean (total / static_cast<float> (samples));

          for (int i (0); i < owner_count; ++i)
          {
            // Registering per owner rather than per iterated chunk matters: an owner across a
            // tile border is written here, and Action dedupes by chunk pointer so this stays
            // one snapshot per chunk per stroke.
            NOGGIT_CUR_ACTION->registerChunkLiquidChange (owners[i]);

            if (owners[i]->liquid_chunk()->setLiquidHeightAt (vertex_x, vertex_z, liquid_id, mean
                                                             , owners[i], opacity_factor))
            {
              mapIndex.setChanged (owners[i]->mt);
              touched.emplace (owners[i]->liquid_chunk());
              changed = true;
            }
          }
        }
      }
    }

    return changed;
  });

  for (ChunkWater* water : touched)
  {
    water->finishLiquidHeightEdit();
  }
}

void World::setWaterType(const TileIndex& pos, int type, int layer)
{
  ZoneScoped;
  for_tile_at ( pos
              , [&] (MapTile* tile)
                {
                  for (int i = 0; i < 16; ++i)
                    for (int j = 0; j < 16; ++j)
                      NOGGIT_CUR_ACTION->registerChunkLiquidChange(tile->getChunk(i, j));

                  tile->Water.setType (type, layer);
                }
              );
}

int World::getWaterType(const TileIndex& tile, int layer) const
{
  ZoneScoped;
  if (mapIndex.tileLoaded(tile))
  {
    return mapIndex.getTile(tile)->Water.getType (layer);
  }
  else
  {
    return 0;
  }
}

void World::autoGenWaterTrans(const TileIndex& pos, float factor)
{
  ZoneScoped;
  for_tile_at(pos, [&](MapTile* tile)
  {
    for (int i = 0; i < 16; ++i)
      for (int j = 0; j < 16; ++j)
        NOGGIT_CUR_ACTION->registerChunkLiquidChange(tile->getChunk(i, j));

    tile->Water.autoGen(factor);
  });
}

void World::CleanupEmptyTexturesChunks()
{
    ZoneScoped;
    for (MapTile* tile : mapIndex.loaded_tiles())
    {
        bool tileChanged = false;

        for (unsigned ty = 0; ty < 16; ty++)
        {
            for (unsigned tx = 0; tx < 16; tx++)
            {
                MapChunk* chunk = tile->getChunk(tx, ty);

                TextureSet* texture_set = chunk->getTextureSet();

                bool changed = texture_set->eraseUnusedTextures();

                if (changed)
                {
                    NOGGIT_CUR_ACTION->registerChunkTextureChange(chunk);
                    tileChanged = true;
                }
            }
        }
        if (tileChanged)
        {
            mapIndex.setChanged(tile);
        }
    }
}

void World::fixAllGaps()
{
  ZoneScoped;
  std::vector<MapChunk*> chunks;

  for (MapTile* tile : mapIndex.loaded_tiles())
  {
    MapTile* left = mapIndex.getTileLeft(tile);
    MapTile* above = mapIndex.getTileAbove(tile);
    bool tileChanged = false;

    // fix the gaps with the adt at the left of the current one
    if (left)
    {
      for (unsigned ty = 0; ty < 16; ty++)
      {
        MapChunk* chunk = tile->getChunk(0, ty);
        NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
        if (chunk->fixGapLeft(left->getChunk(15, ty)))
        {
          chunks.emplace_back(chunk);
          tileChanged = true;
        }
      }
    }

    // fix the gaps with the adt above the current one
    if (above)
    {
      for (unsigned tx = 0; tx < 16; tx++)
      {
        MapChunk* chunk = tile->getChunk(tx, 0);
        NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
        if (chunk->fixGapAbove(above->getChunk(tx, 15)))
        {
          chunks.emplace_back(chunk);
          tileChanged = true;
        }
      }
    }

    // fix gaps within the adt
    for (unsigned ty = 0; ty < 16; ty++)
    {
      for (unsigned tx = 0; tx < 16; tx++)
      {
        MapChunk* chunk = tile->getChunk(tx, ty);
        NOGGIT_CUR_ACTION->registerChunkTerrainChange(chunk);
        bool changed = false;

        // if the chunk isn't the first of the row
        if (tx && chunk->fixGapLeft(tile->getChunk(tx - 1, ty)))
        {
          changed = true;
        }

        // if the chunk isn't the first of the column
        if (ty && chunk->fixGapAbove(tile->getChunk(tx, ty - 1)))
        {
          changed = true;
        }

        if (changed)
        {
          chunks.emplace_back(chunk);
          tileChanged = true;
        }
      }
    }
    if (tileChanged)
    {
      mapIndex.setChanged(tile);
    }
  }

  for (MapChunk* chunk : chunks)
  {
    recalc_norms (chunk);
  }
}

bool World::isUnderMap(glm::vec3 const& pos) const
{
  ZoneScoped;
  TileIndex const tile (pos);

  if (mapIndex.tileLoaded(tile))
  {
    unsigned chnkX = (pos.x / CHUNKSIZE) - tile.x * 16;
    unsigned chnkZ = (pos.z / CHUNKSIZE) - tile.z * 16;

    // check using the cursor height
    return (mapIndex.getTile(tile)->getChunk(chnkX, chnkZ)->getMinHeight()) > pos.y + 2.0f;
  }

  return true;
}

void World::selectVertices(glm::vec3 const& pos, float radius)
{
  ZoneScoped;
  NOGGIT_CUR_ACTION->registerVertexSelectionChange();

  _vertex_center_updated = false;
  _vertex_border_updated = false;

  for_all_chunks_in_range(pos, radius, [&](MapChunk* chunk){
    _vertex_chunks.emplace(chunk);
    _vertex_tiles.emplace(chunk->mt);
    chunk->selectVertex(pos, radius, _vertices_selected);
    return true;
  });

}

bool World::deselectVertices(glm::vec3 const& pos, float radius)
{
  ZoneScoped;
  NOGGIT_CUR_ACTION->registerVertexSelectionChange();

  _vertex_center_updated = false;
  _vertex_border_updated = false;
  std::unordered_set<glm::vec3*> inRange;

  for (glm::vec3* v : _vertices_selected)
  {
    if (misc::dist(*v, pos) <= radius)
    {
      inRange.emplace(v);
    }
  }

  for (glm::vec3* v : inRange)
  {
    _vertices_selected.erase(v);
  }

  return _vertices_selected.empty();
}

void World::moveVertices(float h)
{
  ZoneScoped;
  Noggit::Action* cur_action = NOGGIT_CUR_ACTION;

  assert(cur_action && "moveVertices called without an action running.");

  for (auto& chunk : _vertex_chunks)
    cur_action->registerChunkTerrainChange(chunk);

  _vertex_center_updated = false;
  for (glm::vec3* v : _vertices_selected)
  {
    v->y += h;
  }

  updateVertexCenter();
  updateSelectedVertices();
}

void World::updateSelectedVertices()
{
  ZoneScoped;
  for (MapTile* tile : _vertex_tiles)
  {
    mapIndex.setChanged(tile);
  }

  // fix only the border chunks to be more efficient
  for (MapChunk* chunk : vertexBorderChunks())
  {
    chunk->fixVertices(_vertices_selected);
  }

  for (MapChunk* chunk : _vertex_chunks)
  {
    chunk->registerChunkUpdate(ChunkUpdateFlags::VERTEX);
    recalc_norms (chunk);
  }
}

void World::orientVertices ( glm::vec3 const& ref_pos
                           , math::degrees vertex_angle
                           , math::degrees vertex_orientation
                           )
{
  ZoneScoped;
  Noggit::Action* cur_action = NOGGIT_CUR_ACTION;

  assert(cur_action && "orientVertices called without an action running.");

  for (auto& chunk : _vertex_chunks)
    cur_action->registerChunkTerrainChange(chunk);

  for (glm::vec3* v : _vertices_selected)
  {
    v->y = misc::angledHeight(ref_pos, *v, vertex_angle, vertex_orientation);
  }
  updateSelectedVertices();
}

void World::flattenVertices (float height)
{
  ZoneScoped;
  for (glm::vec3* v : _vertices_selected)
  {
    v->y = height;
  }
  updateSelectedVertices();
}

void World::clearVertexSelection()
{
  ZoneScoped;
  NOGGIT_CUR_ACTION->registerVertexSelectionChange();
  _vertex_border_updated = false;
  _vertex_center_updated = false;
  _vertices_selected.clear();
  _vertex_chunks.clear();
  _vertex_tiles.clear();
}

void World::updateVertexCenter()
{
  ZoneScoped;
  _vertex_center_updated = true;
  _vertex_center = { 0,0,0 };
  float f = 1.0f / _vertices_selected.size();
  for (glm::vec3* v : _vertices_selected)
  {
    _vertex_center += (*v) * f;
  }
}

glm::vec3 const& World::vertexCenter()
{
  ZoneScoped;
  if (!_vertex_center_updated)
  {
    updateVertexCenter();
  }

  return _vertex_center;
}

std::unordered_set<MapChunk*>& World::vertexBorderChunks()
{
  ZoneScoped;
  if (!_vertex_border_updated)
  {
    _vertex_border_updated = true;
    _vertex_border_chunks.clear();

    for (MapChunk* chunk : _vertex_chunks)
    {
      if (chunk->isBorderChunk(_vertices_selected))
      {
        _vertex_border_chunks.emplace(chunk);
      }
    }
  }
  return _vertex_border_chunks;
}
/*
void World::update_models_by_filename()
{
  ZoneScoped;
  _models_by_filename.clear();

  _model_instance_storage.for_each_m2_instance([&] (ModelInstance& model_instance)
  {
    _models_by_filename[model_instance.model->file_key().filepath()].push_back(&model_instance);
    // to make sure the transform matrix are up to date
    model_instance.ensureExtents();
  });

  need_model_updates = false;
}
*/
void World::range_add_to_selection(glm::vec3 const& pos, float radius, bool remove)
{
  ZoneScoped;

  auto objects_in_range = getObjectsInRange(pos, radius);

  for (auto obj : objects_in_range)
  {
    if (remove)
    {
      remove_from_selection(obj, false, false);
    }
    else
    {
      add_to_selection(obj, false, false);
    }
  }
  update_selection_pivot();
}

Noggit::world_model_instances_storage& World::getModelInstanceStorage()
{
  return _model_instance_storage;
}

float World::getMaxTileHeight(const TileIndex& tile)
{
  ZoneScoped;
  MapTile* m_tile = mapIndex.getTile(tile);

  m_tile->forceRecalcExtents();
  float max_height = m_tile->getMaxHeight();

  std::vector<uint32_t>* uids = m_tile->get_uids();

  for (uint32_t uid : *uids)
  {
    auto instance = _model_instance_storage.get_instance(uid);

    if (instance.value().index() == eEntry_Object)
    {
      auto obj = std::get<selected_object_type>(instance.value());
      obj->ensureExtents();
      max_height = std::max(max_height, std::max(obj->getExtents()[0].y, obj->getExtents()[1].y));
    }
  }


  return max_height;
}

SceneObject* World::getObjectInstance(std::uint32_t uid)
{
  ZoneScoped;
  auto instance = _model_instance_storage.get_instance(uid);

  if (!instance)
    return nullptr;

  if (instance.value().index() == eEntry_Object)
  {
    return std::get<selected_object_type>(instance.value());
  }

  return nullptr;
}

void World::setBasename(const std::string &name)
{
  ZoneScoped;
  basename = name;
  mapIndex.set_basename(name);
}


Noggit::VertexSelectionCache World::getVertexSelectionCache()
{
  ZoneScoped;
  return std::move(Noggit::VertexSelectionCache{_vertex_tiles, _vertex_chunks, _vertex_border_chunks,
                                                _vertices_selected, _vertex_center});
}

void World::setVertexSelectionCache(Noggit::VertexSelectionCache& cache)
{
  ZoneScoped;
  _vertex_tiles = cache.vertex_tiles;
  _vertex_chunks = cache.vertex_chunks;
  _vertex_border_chunks = cache.vertex_border_chunks;
  _vertices_selected = cache.vertices_selected;
  _vertex_center = cache.vertex_center;

  _vertex_center_updated = false;
  _vertex_border_updated = false;
}

void World::exportAllADTsAlphamap()
{
  ZoneScoped;
  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);

      if (mTile)
      {
        mTile->wait_until_loaded();

        QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
        if (!(path.endsWith('\\') || path.endsWith('/')))
        {
          path += "/";
        }

        QDir dir(path + "/world/maps/" + basename.c_str());
        if (!dir.exists())
          dir.mkpath(".");

        for (int i = 1; i < 4; ++i)
        {
          QImage img = mTile->getAlphamapImage(i);
          img.save(path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                   + "_" + std::to_string(mTile->index.x).c_str() + "_" + std::to_string(mTile->index.z).c_str()
                   + "_layer" + std::to_string(i).c_str() + ".png", "PNG");
        }

        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
      }
    }
  }
}

void World::exportAllADTsAlphamap(const std::string& filename)
{
  ZoneScoped;
  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);

      if (mTile)
      {
        mTile->wait_until_loaded();

        bool found = false;

        for (int i = 0; i < 16; ++i)
        {
          for (int j = 0; j < 16; ++j)
          {
            auto chunk = mTile->getChunk(i, j);

            for (int k = 1; k < chunk->texture_set->num(); ++k)
            {
              if (chunk->texture_set->filename(k) == filename)
              {
                found = true;
                break;
              }
            }
          }
        }

        if (!found)
          continue;

        QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
        if (!(path.endsWith('\\') || path.endsWith('/')))
        {
          path += "/";
        }

        QDir dir(path + "/world/maps/" + basename.c_str());
        if (!dir.exists())
          dir.mkpath(".");

        QString tex(filename.c_str());
        QImage img = mTile->getAlphamapImage(filename);
        img.save(path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                 + "_" + std::to_string(mTile->index.x).c_str() + "_" + std::to_string(mTile->index.z).c_str()
                 + "_" + tex.replace("/", "-") + ".png", "PNG");

        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
      }
    }
  }
}

void World::exportAllADTsHeightmap()
{
  ZoneScoped;
  float min_height = std::numeric_limits<float>::max();
  float max_height = std::numeric_limits<float>::lowest();

  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);

      if (mTile)
      {
        mTile->wait_until_loaded();

        float max = mTile->getMaxHeight();
        float min = mTile->getMinHeight();

        if (max_height < max)
          max_height = max;

        if (min_height > min)
          min_height = min;

        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
      }
    }
  }

  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);

      if (mTile)
      {
        mTile->wait_until_loaded();

        QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
        if (!(path.endsWith('\\') || path.endsWith('/')))
        {
          path += "/";
        }

        QDir dir(path + "/world/maps/" + basename.c_str());
        if (!dir.exists())
          dir.mkpath(".");

        QImage img = mTile->getHeightmapImage(min_height, max_height);
        img.save(path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                 + "_" + std::to_string(mTile->index.x).c_str() + "_" + std::to_string(mTile->index.z).c_str()
                 + "_height.png", "PNG");

        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
      }
    }
  }
}

void World::exportAllADTsVertexColorMap()
{
  ZoneScoped;
  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);

      if (mTile)
      {
        mTile->wait_until_loaded();

        QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
        if (!(path.endsWith('\\') || path.endsWith('/')))
        {
          path += "/";
        }

        QDir dir(path + "/world/maps/" + basename.c_str());
        if (!dir.exists())
          dir.mkpath(".");

        QImage img = mTile->getVertexColorsImage();
        img.save(path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                 + "_" + std::to_string(mTile->index.x).c_str() + "_" + std::to_string(mTile->index.z).c_str()
                 + "_vcol.png", "PNG");

        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
      }
    }
  }
}

void World::importAllADTsAlphamaps(QProgressDialog* progress_dialog)
{
  bool clean_up = false;
  ZoneScoped;
  QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
  if (!(path.endsWith('\\') || path.endsWith('/')))
  {
    path += "/";
  }
  int count = 0;
  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      if (progress_dialog->wasCanceled())
        return;

      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);
      
      if (mTile)
      {
        mTile->wait_until_loaded();
      
        for (int i = 1; i < 4; ++i)
        {
          QString filename = path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                  + "_" + std::to_string(mTile->index.x).c_str() + "_" + std::to_string(mTile->index.z).c_str()
                  + "_layer" + std::to_string(i).c_str() + ".png";
      
          if(!QFileInfo::exists(filename))
            continue;
      
          QImage img;
          img.load(filename, "PNG");
      
          if (img.width() != 1024 || img.height() != 1024)
          {
            QImage scaled = img.scaled(1024, 1024, Qt::IgnoreAspectRatio);
            mTile->setAlphaImage(scaled, i, clean_up);
          }
          else
          {
            mTile->setAlphaImage(img, i, clean_up);
          }
      
        }
      
        mTile->saveTile(this);
        mapIndex.markOnDisc (tile, true);
        mapIndex.unsetChanged(tile);
      
        if (unload)
        {
          mapIndex.unloadTile(tile);
        }

        count++;
        progress_dialog->setValue(count);
      }

    }
  }
}

void World::importAllADTsHeightmaps(QProgressDialog* progress_dialog, float min_height, float max_height, unsigned mode, bool tiledEdges)
{
  ZoneScoped;
  QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
  if (!(path.endsWith('\\') || path.endsWith('/')))
  {
    path += "/";
  }

  int count = 0;
  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      if (progress_dialog->wasCanceled())
        return;
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);
      
      if (mTile)
      {
        mTile->wait_until_loaded();
      
        QString filename = path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                           + "_" + std::to_string(mTile->index.x).c_str() + "_" + std::to_string(mTile->index.z).c_str()
                           + "_height.png";
      
        if (!QFileInfo::exists(filename))
            continue;
      
        QImage img;
        img.load(filename, "PNG");
      
        size_t desiredSize = tiledEdges ? 256 : 257;
        if (img.width() != desiredSize || img.height() != desiredSize)
        {
          QImage scaled = img.scaled(257, 257, Qt::IgnoreAspectRatio);
          mTile->setHeightmapImage(scaled, min_height, max_height, mode, tiledEdges);
        }
        else
        {
          mTile->setHeightmapImage(img, min_height, max_height, mode, tiledEdges);
        }
      
        mTile->saveTile(this);
        mapIndex.markOnDisc (tile, true);
        mapIndex.unsetChanged(tile);
      
        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
        count++;
        progress_dialog->setValue(count);
      }
    }
  }
}

void World::importAllADTVertexColorMaps(unsigned mode, bool tiledEdges)
{
  ZoneScoped;
  QString path = QString(Noggit::Project::CurrentProject::get()->ProjectPath.c_str());
  if (!(path.endsWith('\\') || path.endsWith('/')))
  {
    path += "/";
  }

  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);

      if (mTile)
      {
        mTile->wait_until_loaded();

        QString filename = path + "/world/maps/" + basename.c_str() + "/" + basename.c_str()
                           + "_" + std::to_string(mTile->index.x).c_str() + "_" + std::to_string(mTile->index.z).c_str()
                           + "_vcol.png";

        if(!QFileInfo::exists(filename))
          continue;

        QImage img;
        img.load(filename, "PNG");

        size_t desiredSize = tiledEdges ? 256 : 257;
        if (img.width() != desiredSize || img.height() != desiredSize)
        {
          QImage scaled = img.scaled(257, 257, Qt::IgnoreAspectRatio);
          mTile->setVertexColorImage(scaled, mode, tiledEdges);
        }
        else
        {
          mTile->setVertexColorImage(img, mode, tiledEdges);
        }

        mTile->saveTile(this);
        mapIndex.markOnDisc (tile, true);
        mapIndex.unsetChanged(tile);

        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
      }
    }
  }
}

void World::ensureAllTilesetsAllADTs()
{
  ZoneScoped;
  static QStringList textures {"tileset/generic/black.blp",
                               "tileset/generic/red.blp",
                               "tileset/generic/green.blp",
                               "tileset/generic/blue.blp",};

  for (size_t z = 0; z < 64; z++)
  {
    for (size_t x = 0; x < 64; x++)
    {
      TileIndex tile(x, z);

      bool unload = !mapIndex.tileLoaded(tile) && !mapIndex.tileAwaitingLoading(tile);
      MapTile* mTile = mapIndex.loadTile(tile);

      if (mTile)
      {
        mTile->wait_until_loaded();

        for (int i = 0; i < 16; ++i)
        {
          for (int j = 0; j < 16; ++j)
          {
            auto chunk = mTile->getChunk(i, j);

            for (int i = 0; i < 4; ++i)
            {
              if (chunk->texture_set->num() <= i)
              {
                scoped_blp_texture_reference tex {textures[i].toStdString(), Noggit::NoggitRenderContext::MAP_VIEW};
                chunk->texture_set->addTexture(tex);
              }
            }

          }
        }

        mTile->saveTile(this);
        mapIndex.markOnDisc (tile, true);
        mapIndex.unsetChanged(tile);

        if (unload)
        {
          mapIndex.unloadTile(tile);
        }
      }
    }
  }
}

void World::notifyTileRendererOnSelectedTextureChange()
{
  ZoneScoped;

  for (MapTile* tile : mapIndex.loaded_tiles())
  {
    tile->renderer()->notifyTileRendererOnSelectedTextureChange();
  }
}

void World::select_objects_in_area(
  const std::array<glm::vec2, 2>& selection_box, 
  bool reset_selection,
  const glm::mat4x4& view,
  const glm::mat4x4& projection,
  int viewport_width, 
  int viewport_height,
  float user_depth,
  const glm::vec3& camera_position)
{
  ZoneScoped;
  
  if (reset_selection)
  {
    this->reset_selection();
  }

  glm::mat4 VPmatrix = projection * view;
  glm::mat4x4 const invertedProjViewMatrix = glm::inverse(VPmatrix);

  constexpr int max_position_raycast_processing = 10000;
  constexpr int max_bounds_raycast_processing = 5000; // when selecting large amount of objects, avoid doing complex ray calculations to not freeze
  constexpr float bounds_check_scale = 0.9f; // size of the bounding box to use when interesecting with selection rectangle
  constexpr float obj_raycast_min_size = 30.0f; // screen size rectangle lenght in pixels

  int processed_obj_count = 0; // num objects that had a raycast test at least once
  // int debug_count_obj_min_size = 0;
  // int debug_count_obj_min_size_not = 0;

  for (auto& map_object : _loaded_tiles_buffer)
  {
    MapTile* tile = map_object.second;

    if (!tile)
    {
      break;
    }

    // some optimizations to see if the tile is in selection before iterating objects in it
    {
      // tile not in screen, skip
      // frustum.intersects(tile_extents[1], tile_extents[0])
      if (!tile->_was_rendered_last_frame)
        continue;

      // check if tile combined extents are within selection rectangle
      // note very useful because cases where a tile is fully rendered and not selected are very rare
 
      // skip if no objects
      if (tile->getObjectInstances().empty())
        continue;

      // bool valid = false;
      // auto screenBounds = misc::getBoundingBoxScreenBounds(tile->getCombinedExtents(), VPmatrix
      //   , viewport_width, viewport_height, valid,  1.0f);
      // 
      // // this only works if all tile points are in screen space
      // if (valid && !math::boxIntersects(screenBounds[0], screenBounds[1]
      //   , selection_box[0], selection_box[1]))
      // {
      //   continue;
      // }
    }

    for (auto const& pair : tile->getObjectInstances())
    {
      [[unlikely]]
      if (!(pair.first->finishedLoading()))
        continue;

      // `|| pair.first->loading_failed()` used to be part of the test above, and it is the
      // reason a placement whose model is missing could not be marquee-selected any more than it
      // could be clicked. Both routes to selecting it were closed, so the only way to remove a
      // broken placement from a map was to edit the ADT outside Noggit.
      //
      // Failed assets are now let through and picked against the placeholder cube that
      // ModelInstance::recalcExtents and WMOInstance::recalcExtents give them. What they are NOT
      // let through to is the refinement pass further down -- see the second guard below.
      bool const load_failed = pair.first->loading_failed();

      [[unlikely]]
      if (pair.second.empty())
        continue;

      SceneObjectTypes objectType = pair.second[0]->which();

      // check if object is hidden
      if (objectType == eWMO)
      {
        WMOInstance* model_instance = static_cast<WMOInstance*>(pair.second[0]);

        if (model_instance->wmo->is_hidden())
          continue;
      }
      else if (objectType == eMODEL)
      {
        ModelInstance* model_instance = static_cast<ModelInstance*>(pair.second[0]);

        if (model_instance->model->is_hidden())
          continue;
      }
      else
      [[unlikely]]
      {
        continue;
      }

      for (auto const& instance : pair.second)
      {
        // problem : M2s have additional sized based culling with >isInRenderDist()
        // if (!instance->_rendered_last_frame)
        //   continue;

        // rectangle selection Pipeline
        // 1 : regular distance checks with object's position
        // 2 : check if oriented bounding box center is in selection rectangle (2D screen projection)
        // 3 : If so, do a raycast that position and check if it's occluded (if any terrain is hit before object center point)
        // if raycast succeeded it's valid !
        // If not, continue : 
        // 4 : check if bounding box is within selection in 2D screen space to test other points (extents screen projection + rectangles intersection)
        //    ! if not, it definitely doesn't intersect, quit.
        // 5 : First, check if object takes enough screenspace, if not and center point failed, it's useless to test more
        // 6 : Now iterate a list of key points from bounding box points to check if they're occluded :
        //    - Optional : get the center of the intersection rectangle (overlap area between obj screen bounds and selection rectangle)
        //                 Project it to 3D and add it to the list of points to check
        //    - First, check if each point is in the selection rectangle (screen projection)
        //    - Now raycast each point and check if each point is occluded by terrain
        //    - if ANY point succeeds and isn't occluded, it means the object isn't entirely occluded and we can select it

        if (processed_obj_count > max_position_raycast_processing)
          break;

        // 1 : regular distance checks with object's position
        const float distance = glm::distance(camera_position, instance->pos);
        if (distance > user_depth || distance > renderer()->cullDistance())
          continue;

        math::aabb obj_world_aabb(instance->getExtents()[0], instance->getExtents()[1]);
        auto aabb_center = obj_world_aabb.center();

        bool point_valid = false;
        auto center_screen_pos = misc::projectPointToScreen(aabb_center, VPmatrix, viewport_width, viewport_height, point_valid);
        // if screenPos.w < 0.0f, object is behind camera
        // check object bounding radius instead to compare the object's size, if it clips with the camera.
        if (center_screen_pos.w < -instance->getBoundingRadius())
        {
          continue;
        }

        bool do_selection = false;
        // 2: check if position point is within rectangle first because it is much cheaper
        {
          const glm::vec2 screenPos2D = glm::vec2(center_screen_pos);
          if (misc::pointInside(screenPos2D, selection_box))
          {
            // processed_obj_count++;
            // 3: check if center point is occluded by terrain
            if (processed_obj_count < max_position_raycast_processing && 
              !is_point_occluded_by_terrain(aabb_center, view, VPmatrix, viewport_width, viewport_height, camera_position))
            {
              // if not occluded success! select it and skip other checks
              add_to_selection(instance, false, false);
              continue;
            }
            // else
            //   bool debug_breakpoint = true;
          }
        }
        
        // Everything from here down refines the hit using the object's LOCAL bounding box and
        // its transform matrix, and for a failed asset both are indeterminate: Model's
        // bounding_box_min/max and WMO::extents are never written when loading throws
        // (Model.cpp:19-24 has the constructor memset commented out, and GLM_FORCE_CTOR_INIT is
        // not defined in this tree), and ModelInstance::recalcExtents returns before
        // updateTransformMatrix() in the failed case so _transform_mat is the uninitialised
        // default from SceneObject.hpp:94. getBoundingBoxScreenBounds on those values selects
        // arbitrary objects, or none, depending on stack contents.
        //
        // The centre test above is sufficient on its own here: the placeholder is a small cube
        // and its centre is the whole of it. Refining it would add nothing even if the data were
        // sound.
        if (load_failed)
          continue;

        // 4 : if center point raycast didn't succeed, check again if bounding box is within selection in 2D screen space to test other points

        std::array<glm::vec3, 2> local_extents;
        if (objectType == eWMO)
        {
          WMOInstance* wmo_instance = static_cast<WMOInstance*>(pair.second[0]);
          local_extents = wmo_instance->getLocalExtents();
        }
        else if (objectType == eMODEL)
        {
          ModelInstance* model_instance = static_cast<ModelInstance*>(pair.second[0]);
          local_extents = model_instance->getLocalExtents();
        }

        int num_valid_points = 0;
        std::array<glm::vec2, 2> obj_screnbounds = misc::getBoundingBoxScreenBounds(local_extents, VPmatrix
          , viewport_width, viewport_height, num_valid_points, instance->transformMatrix(), bounds_check_scale);

        if (num_valid_points < 3)
          continue;

        // Screen bounds intersection check 
        // 
        // if (!math::boxIntersects(obj_screnbounds[0], obj_screnbounds[1]
        //   , selection_box[0], selection_box[1]))
        // {
        //   // if rectangles don't intersect, just skip
        //   continue;
        // }

        // 1 : get the intersection rectangle of screen space and bounding box
        glm::vec2 intersectionMin = glm::max(obj_screnbounds[0], selection_box[0]);
        glm::vec2 intersectionMax = glm::min(obj_screnbounds[1], selection_box[1]);
        // Check for Valid Intersection:
        // if (intersectionMin.x < intersectionMax.x && intersectionMin.y < intersectionMax.y)
        if (!(intersectionMin.x < intersectionMax.x) || !(intersectionMin.y < intersectionMax.y))
          continue;

        // 2 : get center
        glm::vec2 intersectionCenter = (intersectionMin + intersectionMax) * 0.5f;
        // 3 : convert 2D screenspace point back to 3d
        glm::vec4 normalisedView = invertedProjViewMatrix * misc::normalized_device_coords(intersectionCenter.x, intersectionCenter.y,
          viewport_width, viewport_height);
        glm::vec3 intersectionCenter_pos = glm::vec3(normalisedView.x / normalisedView.w, normalisedView.y / normalisedView.w, normalisedView.z / normalisedView.w);

        std::array<glm::vec3, 8> obj_world_bounds_corners;
        // For animated models, recalc vertex bounding box
        if (objectType == eMODEL)
        {
          ModelInstance* model_instance = static_cast<ModelInstance*>(pair.second[0]);
          if (model_instance->model->animated_mesh() && model_instance->model->mesh_bounds_ratio < 0.8f)
          {
            auto animated_local_extents = model_instance->model->getAnimatedBoundingBox();

            // hack, animated coords are already adjusted
            animated_local_extents[0] = glm::vec3(animated_local_extents[0].x, -animated_local_extents[0].z, animated_local_extents[0].y);
            animated_local_extents[1] = glm::vec3(animated_local_extents[1].x, -animated_local_extents[1].z, animated_local_extents[1].y);

            // update screen bounds
            num_valid_points = 0;
            obj_screnbounds = misc::getBoundingBoxScreenBounds(animated_local_extents, VPmatrix
              , viewport_width, viewport_height, num_valid_points, instance->transformMatrix(), bounds_check_scale);


            // check if animated BB intersected
            if (num_valid_points < 3)
              continue;
            if (!math::boxIntersects(obj_screnbounds[0], obj_screnbounds[1]
              , selection_box[0], selection_box[1]))
            {
              continue;
            }

            math::aabb animated_local_aabb(animated_local_extents[0], animated_local_extents[1]);
            // converts to world
            obj_world_bounds_corners = animated_local_aabb.rotated_corners(instance->transformMatrix(), true);

            // get extents and update bb to use
            obj_world_aabb = math::aabb(std::vector<glm::vec3>(obj_world_bounds_corners.begin(), obj_world_bounds_corners.end()));

            // raycast the center of the intersecting animated bounds
            // 
            // get the center of the intersection rectangle
            // 1 : get the intersection rectangle of screen space and bounding box
            intersectionMin = glm::max(obj_screnbounds[0], selection_box[0]);
            intersectionMax = glm::min(obj_screnbounds[1], selection_box[1]);
            // Check for Valid Intersection:
            if (intersectionMin.x < intersectionMax.x && intersectionMin.y < intersectionMax.y) {
              // Valid intersection
            }
            else 
            {
              continue;
            }
            // 2 : get center
            intersectionCenter = (intersectionMin + intersectionMax) * 0.5f;
            // 3 : convert 2D screenspace point back to 3d
            normalisedView = invertedProjViewMatrix * misc::normalized_device_coords(intersectionCenter.x, intersectionCenter.y,
              viewport_width, viewport_height);
            intersectionCenter_pos = glm::vec3(normalisedView.x / normalisedView.w, normalisedView.y / normalisedView.w, normalisedView.z / normalisedView.w);
            //////

          }
          else
          {
            obj_world_bounds_corners = obj_world_aabb.rotated_corners(instance->transformMatrix(), false);
          }
        }
        else if (objectType == eWMO)
        {
          obj_world_bounds_corners = obj_world_aabb.rotated_corners(instance->transformMatrix(), false);
        }


        // 4.5 2nd raycast. Check if center of the intersection box is visible
        // TODO : for WMOs this is way to generous due to their more complex shape, it would be better to iterate the bounding box of each group
        if (!is_point_occluded_by_terrain(intersectionCenter_pos, view, VPmatrix, viewport_width, viewport_height
          , camera_position, (distance - instance->getBoundingRadius())))
        {
          // if not occluded success! select it and skip other checks
          add_to_selection(instance, false, false);
          continue;
        }

        // 5 : Optimization : Only do raycast bounds checks for object that take enough screen space
        // if object is too small checking other points is useless
        // we check _rendered_last_frame because m2s that are too small or frustum culled already don't render
        {
          float bounds_size = glm::distance(obj_screnbounds[0], obj_screnbounds[1]);
          if (bounds_size < obj_raycast_min_size || !instance->_rendered_last_frame)
          {
            // debug_count_obj_min_size_not++;
            continue;
          }
          else if (processed_obj_count > max_bounds_raycast_processing)
          {
            // select it anyways
            do_selection = true;
            // debug_count_obj_min_size++;
          }
        }

        constexpr bool enable_bounds_raycasts = true;
        //6 : Occlusion test on object's corners (that are in selection box)
        // uses ray casting, very expensive
        if (enable_bounds_raycasts && !do_selection /* && instance->_rendered_last_frame && (processed_obj_count < max_bounds_raycast_processing)*/)
        {
          processed_obj_count++;

          // TODO : instead iterate bounds of the intersection rectangle instead of object's bounds

          // Iterate key points instead of all 8 corners
          std::vector<glm::vec3> key_points = {
            // intersectionCenter_pos, // checked in 4.5 now
            // (obj_world_bounds_corners[0] + obj_world_bounds_corners[6]) * 0.5f,  // Center between top corners
            // obj_world_bounds_corners[0], // Top-right-front
            // obj_world_bounds_corners[5], // Top-left-back
            // obj_world_bounds_corners[4], // Top-left-front
            // obj_world_bounds_corners[1] // Top-right-back
          };

          // int required_num_unoccluded_corners = 2;
          bool object_occluded = true;

          // check if points are occluded by terrain
          // bool first_point = true;// special for intersectionCenter_pos because it doesn't have a distance, just a direction

          for (const auto& corner : key_points /*obj_aabb_corners*/)
          {
            // TODO : only need to do max top left and max top right in 2d instead of all corners?

            // only process points that are within selection rectangle
            bool point_valid = false;
            auto point_screen_pos = misc::projectPointToScreen(corner, VPmatrix, viewport_width, viewport_height, point_valid);
            if (!point_valid)
              continue;
            if (!misc::pointInside(point_screen_pos, selection_box))
              continue;

            bool corner_occluded = is_point_occluded_by_terrain(corner
              , view
              , VPmatrix
              , viewport_width
              , viewport_height
              , camera_position
              /*, first_point ? distance - instance->getBoundingRadius() : 0.0f*/);

            // first_point = false;

            if (!corner_occluded)
            {
              // if just one point isn't occluded is enough, select object
              object_occluded = false;
              break;
            }
            // object_occluded = true;
          }
        
          do_selection = !object_occluded;
        }

        if (!do_selection)
          continue;

        add_to_selection(instance, false, false);
      }
    }
  }

  this->update_selection_pivot();
}


bool World::is_point_occluded_by_terrain(const glm::vec3& point,
  const glm::mat4x4& view,
  const glm::mat4& VPmatrix,
  float viewport_width,
  float viewport_height,
  const glm::vec3& camera_position,
  float distance_override
  )
{
  /*
  bool point_valid = false;
  auto point_screen_pos = misc::projectPointToScreen(point, VPmatrix, viewport_width, viewport_height, point_valid);
  
  if (!point_valid)
  {
    return true;
  }*/

  math::ray ray(camera_position, point - camera_position); // 3d display mode only.

  // intersect only terrain with a ray to object's position
  selection_result terrain_intersect_results
  (intersect
  (glm::transpose(view)
    , ray
    , true
    , false
    , true
    , false
    , false
    , false
    , false
    , false
  )
  );

  float distance = distance_override == 0.0f ? glm::distance(camera_position, point) : distance_override;

  // bool point_occluded = false;
  for (const auto& terrain_hit : terrain_intersect_results)
  {
    // if terrain hit is further, skip
    if (terrain_hit.first + 5.0f > distance) // add some leeway, skip hits that are too close, especially for the terrain at object's origin
      continue;

    return true;
  }

  // no terrain intersection point above point
  return false;
}

void World::add_object_group_from_selection()
{
    // create group from selected objects
    selection_group selection_group(get_selected_objects(), this);
    selection_group._is_selected = true;

    _selection_groups.push_back(selection_group);

    // write group to project
    saveSelectionGroups();
}

/*
void World::remove_selection_group(selection_group* group)
{
   std::vector<selection_type>::iterator position = std::find(_selection_groups.begin(), _selection_groups.end(), group);
   if (position != _selection_groups.end())
   {
       _selection_groups.erase(position);
   }

   for (auto it = _selection_groups.begin(); it != _selection_groups.end(); ++it)
   {
       auto it_group = *it;
       if (it_group.getMembers().size() == group->getMembers().size() && it_group.getExtents() == group->getExtents())
       // if (it_group.isSelected())
       {
           _selection_groups.erase(it);
           saveSelectionGroups();
           return;
       }
   }
}*/

void World::clear_selection_groups()
{
    for (auto& group : _selection_groups)
    {
        // auto it_group = *it;
        // it->remove_group();
        group.remove_group(false);
    }
    _selection_groups.clear(); // in case it didn't properly clear
    saveSelectionGroups(); // only save once
}

