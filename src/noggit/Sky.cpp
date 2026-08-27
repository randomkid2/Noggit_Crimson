// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/DBC.h>
#include <noggit/Log.h>
#include <noggit/MapHeaders.h>
#include <noggit/Model.h> // Model
#include <noggit/ModelManager.h> // ModelManager
#include <noggit/Sky.h>
#include <noggit/application/NoggitApplication.hpp>
#include <noggit/application/Configuration/NoggitApplicationConfiguration.hpp>
#include <opengl/shader.hpp>
#include <glm/glm.hpp>
#include <noggit/project/CurrentProject.hpp>
#include <math/trig.hpp>
#include <math/bounding_box.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <QFile>
#include <QTextStream>
#include <QStringList>

const float skymul = 36.0f;

namespace
{
  // The LightIntBand.dbc and LightFloatBand.dbc row ids a LightParams id owns.
  //
  // Not a lookup: neither band table has a column pointing back at its param. The relation is pure
  // arithmetic -- 18 colour rows and 6 float rows per param -- so LightParams 1 owns colour rows
  // 1..18 and float rows 1..6, LightParams 2 owns 19..36 and 7..12, and so on. The reader
  // (SkyParam's constructor) and the writer (Sky::save_to_dbc) each spelled the same two
  // expressions out inline; they live here once now, because getting one of them wrong writes a
  // whole param's colours over its neighbour's.
  int lightIntBandFirstRow(int param_id)
  {
    return (param_id * NUM_SkyColorNames) - (NUM_SkyColorNames - 1);
  }

  int lightFloatBandFirstRow(int param_id)
  {
    return (param_id * NUM_SkyFloatParamsNames) - (NUM_SkyFloatParamsNames - 1);
  }
}

namespace skyparams
{
  // Map to store SkyParams globally
  std::unordered_map<unsigned int, SkyParam> skyParamMap;
  
  // get or create SkyParam from the global map
  SkyParam* getOrCreateParam(unsigned int id, Noggit::NoggitRenderContext context) {
  
    if (!id)
      return nullptr;
  
    auto it = skyParamMap.find(id);
    if (it != skyParamMap.end()) {
      return &(it->second);  // Return existing SkyParam
    }
  
    // Try to create new SkyParam and insert into map
    SkyParam newParam = SkyParam(id, context);
  
    // Dbc loading failed
    assert(newParam.Id != 0);
    if (newParam.Id == 0)
      return nullptr;
  
    auto [newIt, inserted] = skyParamMap.emplace(id, std::move(newParam));
    return &(newIt->second);  // Return newly created SkyParam
  }

  // Loads `source_id` afresh and registers the result under `new_id` as an unsaved record.
  //
  // Reloaded from the DBC rather than copy-constructed from the cached SkyParam because
  // SkyParam owns a std::optional<ModelInstance> for the skybox: the constructor and the
  // emplace above are the only two operations the type is known to support today (one build,
  // one move), and reusing exactly those keeps a deep copy from depending on a copy constructor
  // nothing else in the tree exercises.
  //
  // Is this LightParams id spoken for, either on disk or by a param already loaded in memory?
  //
  // Both halves matter. The DBC answers for Blizzard's rows; the map answers for rows that a paste
  // has created but nobody has saved yet, and those are invisible to the DBC by design.
  bool paramIdIsRegistered(unsigned int id)
  {
    return skyParamMap.find(id) != skyParamMap.end();
  }

  // Returns nullptr when the source cannot be read or `new_id` is already taken.
  SkyParam* createParamCopy(unsigned int source_id, unsigned int new_id, Noggit::NoggitRenderContext context)
  {
    if (!source_id || !new_id || skyParamMap.find(new_id) != skyParamMap.end())
      return nullptr;

    SkyParam copy = SkyParam(source_id, context);

    if (copy.Id == 0) // SkyParam's constructor zeroes Id when the LightParams row would not read
      return nullptr;

    copy.Id = new_id;

    // Everything about this record is new: the LightParams row, its 18 LightIntBand rows and its
    // 6 LightFloatBand rows all have to be ADDED rather than updated, and _is_new_param_record is
    // the single flag save_to_dbc reads to decide addRecord vs getByID for all 25 of them.
    copy._is_new_param_record = true;
    copy._need_save = true;
    copy._colors_need_save = true;
    copy._floats_need_save = true;

    auto [it, inserted] = skyParamMap.emplace(new_id, std::move(copy));
    return inserted ? &(it->second) : nullptr;
  }
}

namespace Noggit
{
  namespace
  {
    // One clipboard for the whole process, so a copy in Stormwind survives loading a custom map.
    LightSnapshot g_light_clipboard;
  }

  LightSnapshot const& lightClipboard()
  {
    return g_light_clipboard;
  }

  void setLightClipboard(LightSnapshot const& snapshot)
  {
    g_light_clipboard = snapshot;
  }

  bool lightSnapshotFromDbc(int light_id, LightSnapshot& out)
  {
    if (light_id <= 0)
    {
      return false;
    }

    try
    {
      DBCFile::Record record = gLightDB.getByID(static_cast<unsigned int>(light_id), LightDB::ID);

      out = LightSnapshot();
      out.valid = true;
      out.light_id = light_id;
      out.map_id = record.getInt(LightDB::Map);
      out.pos = glm::vec3( record.getFloat(LightDB::PositionX) / skymul
                         , record.getFloat(LightDB::PositionY) / skymul
                         , record.getFloat(LightDB::PositionZ) / skymul
                         );
      out.r1 = record.getFloat(LightDB::RadiusInner) / skymul;
      out.r2 = record.getFloat(LightDB::RadiusOuter) / skymul;

      for (int i = 0; i < NUM_SkyParamsNames; ++i)
      {
        out.param_ids[i] = record.getUInt(LightDB::DataIDs + i);
      }

      return true;
    }
    catch (...)
    {
      LogError << "Could not read Light.dbc row " << light_id << " for the light clipboard."
               << std::endl;
      return false;
    }
  }
}


SkyColor::SkyColor(int t, int col)
{
  time = t;
  color.z = ((col & 0x0000ff)) / 255.0f;
  color.y = ((col & 0x00ff00) >> 8) / 255.0f;
  color.x = ((col & 0xff0000) >> 16) / 255.0f;
}

SkyFloatParam::SkyFloatParam(int t, float val)
: time(t)
, value(val)
{
}

SkyParam::SkyParam(int paramId, Noggit::NoggitRenderContext context)
: _context(context)
{
  Id = paramId;

  if (Id == 0)
  {
    // shouldn't happen in the new system, we don't load params with no valid id.
    assert(false);

    return; // don't initialise entry
  }

  try
  {
    DBCFile::Record light_param = gLightParamsDB.getByID(paramId);
    int skybox_id = light_param.getInt(LightParamsDB::skybox);

    _highlight_sky = light_param.getInt(LightParamsDB::highlightSky);
    _river_shallow_alpha = light_param.getFloat(LightParamsDB::water_shallow_alpha);
    _river_deep_alpha = light_param.getFloat(LightParamsDB::water_deep_alpha);
    _ocean_shallow_alpha = light_param.getFloat(LightParamsDB::ocean_shallow_alpha);
    _ocean_deep_alpha = light_param.getFloat(LightParamsDB::ocean_deep_alpha);
    _glow = light_param.getFloat(LightParamsDB::glow);

    if (skybox_id)
    {
      try
      {
        auto skyboxRec = gLightSkyboxDB.getByID(skybox_id);
        skybox.emplace(skyboxRec.getString(LightSkyboxDB::filename), _context);
        skyboxFlags = skyboxRec.getInt(LightSkyboxDB::flags);
      }
      catch (...)
      {
        LogError << "When trying to get the skybox id " << skybox_id << "for the param " << paramId << " in LightSkybox.dbc." << std::endl;
      }

    }
  }
  catch (...)
  {
    LogError << "When trying to initialize Params the entry " << paramId << " in LightParams.dbc." << std::endl;
    Id = 0;
  }

  // initialize colors (lightIntBand.dbc)
  int light_int_start = lightIntBandFirstRow(paramId);
  for (int i = 0; i < NUM_SkyColorNames; ++i)
  {
    try
    {
      DBCFile::Record rec = gLightIntBandDB.getByID(light_int_start + i);
      int entries = rec.getInt(LightIntBandDB::Entries);

      if (entries == 0)
      {
        // mmin[i] = -1;
      }
      else
      {
        // smallest/first time value
        // mmin[i] = rec.getInt(LightIntBandDB::Times);
        for (int l = 0; l < entries; l++)
        {
            SkyColor sc(rec.getInt(LightIntBandDB::Times + l), rec.getInt(LightIntBandDB::Values + l));
            colorRows[i].push_back(sc);
        }
      }
    }
    catch (...)
    {
      assert(false);
      LogError << "When trying to intialize sky, there was an error with getting an entry in LightIntBand.dbc id (" << i << "). Lightparam id : " << paramId << std::endl;
      /*
      DBCFile::Record rec = gLightIntBandDB.getByID(i);
      int entries = rec.getInt(LightIntBandDB::Entries);

      if (entries == 0)
      {
          mmin[i] = -1;
      }
      else
      {
          mmin[i] = rec.getInt(LightIntBandDB::Times);
          for (int l = 0; l < entries; l++)
          {
              SkyColor sc(rec.getInt(LightIntBandDB::Times + l), rec.getInt(LightIntBandDB::Values + l));
              colorRows[i].push_back(sc);
          }
      }*/
    }
  }

  // initialize float params
  int light_float_start = lightFloatBandFirstRow(paramId);
  for (int i = 0; i < NUM_SkyFloatParamsNames; ++i)
    {
        try
        {
            DBCFile::Record rec = gLightFloatBandDB.getByID(light_float_start + i);
            int entries = rec.getInt(LightFloatBandDB::Entries);

            if (entries == 0)
            {
                // mmin_float[i] = -1;
            }
            else
            {
                // mmin_float[i] = rec.getInt(LightFloatBandDB::Times);
                for (int l = 0; l < entries; l++)
                {
                    SkyFloatParam sc(rec.getInt(LightFloatBandDB::Times + l), rec.getFloat(LightFloatBandDB::Values + l));
                    floatParams[i].push_back(sc);
                }
            }
        }
        catch (...)
        {
            assert(false);
            LogError << "When trying to intialize sky, there was an error with getting an entry in LightFloatBand.dbc id (" << i << "). Lightparam id : " << paramId << std::endl;
            /*
            DBCFile::Record rec = gLightFloatBandDB.getByID(i + 1);
            int entries = rec.getInt(LightFloatBandDB::Entries);

            if (entries == 0)
            {
                mmin_float[i] = -1;
            }
            else
            {
                mmin_float[i] = rec.getInt(LightFloatBandDB::Times);
                for (int l = 0; l < entries; l++)
                {
                    SkyFloatParam sc(rec.getInt(LightFloatBandDB::Times + l), rec.getFloat(LightFloatBandDB::Values + l));
                    floatParams[i].push_back(sc);
                }
            }*/
        }
    }

}

bool SkyParam::highlight_sky() const
{
  return _highlight_sky;
}

float SkyParam::river_shallow_alpha() const
{
  return _river_shallow_alpha;
}

float SkyParam::river_deep_alpha() const
{
  return _river_deep_alpha;
}

float SkyParam::ocean_shallow_alpha() const
{
  return _ocean_shallow_alpha;
}

float SkyParam::ocean_deep_alpha() const
{
  return _ocean_deep_alpha;
}

float SkyParam::glow() const
{
  return _glow;
}

void SkyParam::set_glow(float glow)
{
  _glow = glow;
}

void SkyParam::set_highlight_sky(bool state)
{
  _highlight_sky = state;
}

void SkyParam::set_river_shallow_alpha(float alpha)
{
  _river_shallow_alpha = alpha;
}

void SkyParam::set_river_deep_alpha(float alpha)
{
  _river_deep_alpha = alpha;
}

 void SkyParam::set_ocean_shallow_alpha(float alpha)
{
  _ocean_shallow_alpha = alpha;
}

void SkyParam::set_ocean_deep_alpha(float alpha)
{
  _ocean_deep_alpha = alpha;
}


Sky::Sky(DBCFile::Iterator data, Noggit::NoggitRenderContext context)
: _context(context)
, _selected(false)
{
  Id = data->getInt(LightDB::ID);
  _map_id = data->getInt(LightDB::Map);
  pos = glm::vec3(data->getFloat(LightDB::PositionX) / skymul, data->getFloat(LightDB::PositionY) / skymul, data->getFloat(LightDB::PositionZ) / skymul);
  r1 = data->getFloat(LightDB::RadiusInner) / skymul;
  r2 = data->getFloat(LightDB::RadiusOuter) / skymul;

  global = (pos.x == 0.0f && pos.y == 0.0f && pos.z == 0.0f);

  for (int i = 0; i < NUM_SkyParamsNames; ++i)
  {
      int sky_param_id = data->getInt(LightDB::DataIDs + i);

      skyParams[i] = sky_param_id;

      // initialize param to map, shouldn't even be needed
      // if (sky_param_id > 0)
      // {
      //   getOrCreateParam(sky_param_id, _context);
      // }
  }
}

Sky::Sky( Noggit::LightSnapshot const& snapshot
        , int new_id
        , int map_id
        , Noggit::NoggitRenderContext context
        )
: _context(context)
, _selected(false)
{
  Id = new_id;
  _map_id = map_id;
  pos = snapshot.pos;
  r1 = snapshot.r1;
  r2 = snapshot.r2;

  // The same rule the DBC constructor uses: a light at the origin is the map's global light. A
  // paste at 0,0,0 would therefore silently become a second global light, which the client cannot
  // express -- LightEditor refuses that before it gets here, and this stays consistent either way.
  global = (pos.x == 0.0f && pos.y == 0.0f && pos.z == 0.0f);

  for (int i = 0; i < NUM_SkyParamsNames; ++i)
  {
    skyParams[i] = snapshot.param_ids[i];
  }

  is_new_record = true;
}

int Sky::getId() const
{
  return Id;
}

int Sky::mapId() const
{
  return _map_id;
}

void Sky::setMapId(int map_id)
{
  _map_id = map_id;
}

void Sky::setSelected(bool selected)
{
  _selected = selected;
}

std::optional<SkyParam*> Sky::getParam(int param_index) const
{
  unsigned int param_id = skyParams[param_index];
  if (param_id == 0)
    return std::nullopt;
  
  if (cachedCurrentParam && cachedCurrentParam->Id == param_id) {
    return cachedCurrentParam;
  }
  
  SkyParam* param_ptr = skyparams::getOrCreateParam(param_id, _context);

  if (param_ptr)
  {
    cachedCurrentParam = param_ptr;
    return cachedCurrentParam;
  }
  else
  {
    cachedCurrentParam = nullptr;
    return std::nullopt;
  }
}

std::optional<SkyParam*> Sky::getCurrentParam() const
{
  return getParam(curr_sky_param);
}

float Sky::floatParamFor(int r, int t) const
{
  auto param_opt = getCurrentParam();
  if (!param_opt.has_value())
    return 0.0f;

  SkyParam* const sky_param = param_opt.value();

  if (sky_param->floatParams[r].empty())
  {
    return 0.0f;
  }
  float c1, c2;
  int t1, t2;
  size_t last = sky_param->floatParams[r].size() - 1;

  if (t < sky_param->floatParams[r].front().time)
  {
    // reverse interpolate
    c1 = sky_param->floatParams[r][last].value;
    c2 = sky_param->floatParams[r][0].value;
    t1 = sky_param->floatParams[r][last].time;
    t2 = sky_param->floatParams[r][0].time + DAY_DURATION;
    t += DAY_DURATION;
  }
  else
  {
    for (size_t i = last; true; i--)
    { //! \todo iterator this.
      if (sky_param->floatParams[r][i].time <= t)
      {
        c1 = sky_param->floatParams[r][i].value;
        t1 = sky_param->floatParams[r][i].time;

        if (i == last)
        {
          c2 = sky_param->floatParams[r][0].value;
          t2 = sky_param->floatParams[r][0].time + DAY_DURATION;
        }
        else
        {
          c2 = sky_param->floatParams[r][i + 1].value;
          t2 = sky_param->floatParams[r][i + 1].time;
        }
        break;
      }
    }
  }

  float tt = static_cast<float>(t - t1) / static_cast<float>(t2 - t1);
  return c1 + ((c2 - c1) * tt);
}

glm::vec3 Sky::colorFor(int r, int t) const
{
  auto param_opt = getCurrentParam();
  if (!param_opt.has_value())
    return glm::vec3(0.0f, 0.0f, 0.0f);

  SkyParam* const sky_param = param_opt.value();

  if (sky_param->colorRows[r].empty())
  {
    return glm::vec3(0.0f, 0.0f, 0.0f);
  }
  glm::vec3 c1, c2;
  int t1, t2;
  int last = static_cast<int>(sky_param->colorRows[r].size()) - 1;

  if (last == 0)
  {
      c1 = sky_param->colorRows[r][last].color;
      c2 = sky_param->colorRows[r][0].color;
      t1 = sky_param->colorRows[r][last].time;
      t2 = sky_param->colorRows[r][0].time + DAY_DURATION;
      t += DAY_DURATION;
  }
  else
  {

      // if (t < sky_param->mmin[r])
      if (t < sky_param->colorRows[r].front().time)
      {
          // reverse interpolate
          c1 = sky_param->colorRows[r][last].color;
          c2 = sky_param->colorRows[r][0].color;
          t1 = sky_param->colorRows[r][last].time;
          t2 = sky_param->colorRows[r][0].time + DAY_DURATION;
          t += DAY_DURATION;
      }
      else
      {
          for (int i = last; true; i--)
          { //! \todo iterator this.
              if (sky_param->colorRows[r][i].time <= t)
              {
                  c1 = sky_param->colorRows[r][i].color;
                  t1 = sky_param->colorRows[r][i].time;

                  if (i == last)
                  {
                      c2 = sky_param->colorRows[r][0].color;
                      t2 = sky_param->colorRows[r][0].time + DAY_DURATION;
                  }
                  else
                  {
                      c2 = sky_param->colorRows[r][i + 1].color;
                      t2 = sky_param->colorRows[r][i + 1].time;
                  }
                  break;
              }
          }
      }
  }

  float tt = static_cast<float>(t - t1) / static_cast<float>(t2 - t1);
  return c1*(1.0f - tt) + c2*tt;
}

const float rad = 400.0f;

//...............................top....med....medh........horiz..........bottom
const math::degrees angles[] = { math::degrees (90.0f)
                               , math::degrees (18.0f)
                               , math::degrees (10.0f)
                               , math::degrees (3.0f)
                               , math::degrees (0.0f)
                               , math::degrees (-30.0f)
                               , math::degrees (-90.0f)
                               };
const int cnum = 7;
const int skycolors[cnum] = { SKY_COLOR_TOP, SKY_COLOR_MIDDLE, SKY_COLOR_BAND1, SKY_COLOR_BAND2, SKY_COLOR_SMOG, SKY_FOG_COLOR, SKY_FOG_COLOR };
const int hseg = 32;


void Skies::loadZoneLights(int map_id)
{
    // read zone lights from csv file
  {
    std::string zonelight_db_path = Noggit::Application::NoggitApplication::instance()->getConfiguration()->ApplicationNoggitDefinitionsPath
      + "\\ZoneLight.3.4.3.56262.csv";
    QString qPath = QString::fromStdString(zonelight_db_path);
    QFile file(qPath);

    if (file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
      QTextStream in(&file);

      // Skip the header line
      std::string headerLine = in.readLine().toStdString();
      assert(headerLine == "ID,Name,MapID,LightID");

      while (!in.atEnd())
      {
        QString line = in.readLine();

        // Split the line by comma
        QStringList fields = line.split(',');

        assert(fields.size() == 4);
        if (fields.size() != 4)
        {
          continue;
        }

        bool ok;
        int light_map_id = fields[2].toInt(&ok);
        assert(ok);
        // only load this map
        if (map_id != light_map_id)
          continue;

        ZoneLight zone_light_entry;
        zone_light_entry.id = fields[0].toInt(&ok);
        zone_light_entry.name = fields[1].toStdString();
        // zone_light_entry.mapId = light_map_id;
        zone_light_entry.lightId = fields[3].toInt(&ok);

        // zoneLightsWotlk[zone_light_entry.id] = zone_light_entry;
        zoneLightsWotlk.push_back(zone_light_entry);

        // get the light reference
        Sky* light_ptr = findSkyById(zone_light_entry.lightId);
        // if light was not loaded, most likely missing or not in map.
        assert(light_ptr != nullptr);
        if (!light_ptr)
          continue;
        // zoneLightsWotlk[zone_light_entry.id].light = findSkyById(zone_light_entry.lightId);
        light_ptr->zone_light = true;
      }
      file.close();
    }
    else
    {
      LogError << "Failed loading Zone Lights. Can't open " << zonelight_db_path << std::endl;
      return;
    }
  }

  // load zone light points to temporary object
  std::unordered_map<int, std::vector<ZoneLightPoint>> zoneLightPoints;
  {
    std::string zonelightpoints_db_path = Noggit::Application::NoggitApplication::instance()->getConfiguration()->ApplicationNoggitDefinitionsPath
      + "\\ZoneLightPoint.3.4.3.56262.csv";
    QString qPath = QString::fromStdString(zonelightpoints_db_path);
    QFile file(qPath);

    if (file.open(QIODevice::ReadOnly | QIODevice::Text))
    {
      QTextStream in(&file);

      // Skip the header line
      std::string headerLine = in.readLine().toStdString();
      assert(headerLine == "ID,Pos_0,Pos_1,PointOrder,ZoneLightID");

      while (!in.atEnd())
      {
        QString line = in.readLine();

        // Split the line by comma
        QStringList fields = line.split(',');

        assert(fields.size() == 5);
        if (fields.size() != 5)
        {
          continue;
        }

        bool ok;
        int zone_light_id = fields[4].toUInt(&ok);
        assert(ok);

        // Check if the vector contains the zone_light_id
        // if (!zoneLightsWotlk.contains(zone_light_id))
        auto it = std::find_if(zoneLightsWotlk.begin(), zoneLightsWotlk.end(),
          [zone_light_id](const ZoneLight& zoneLight) {
            return zoneLight.id == zone_light_id;
          });
        if (it == zoneLightsWotlk.end())
          continue;

        ZoneLightPoint zone_light_point_entry;
        zone_light_point_entry.id = fields[0].toUInt(&ok);
        zone_light_point_entry.zoneLightId = zone_light_id;

        // convert server to client coords
        // need to swap X and Y
        zone_light_point_entry.posX = -fields[2].toFloat(&ok) + ZEROPOINT;
        zone_light_point_entry.posY = -fields[1].toFloat(&ok) + ZEROPOINT;
        zone_light_point_entry.pointOrder = fields[3].toUInt(&ok);

        // Automatically create a vector if the key doesn't exist, and add the entry
        zoneLightPoints[zone_light_id].push_back(zone_light_point_entry);

        // bad idea, this blindly trusts the ordering
        // zoneLightsWotlk[zone_light_id].points.push_back(glm::vec2(zone_light_point_entry.posX, zone_light_point_entry.posY));

      }
      file.close();
    }
    else
    {
      LogError << "Failed loading Zone Light Points. Can't open " << zonelightpoints_db_path << std::endl;
      return;
    }
  }

  // re order points, because we don't trust the storage order
  for (auto& points_list : zoneLightPoints)
  {
    // if (!zoneLightsWotlk.contains(points_list.first))
    //   continue;

    // polygon must have at least 3 points
    assert(points_list.second.size() > 2);

    // Check for duplicate pointOrder values
    for (int i = 1; i < points_list.second.size(); ++i) {
      if (points_list.second[i].pointOrder == points_list.second[i - 1].pointOrder)
      {
        assert(false);
        continue;
      }
    }

    // Sort the vector based on pointOrder (ascending)
    std::sort(points_list.second.begin(), points_list.second.end(), [](const ZoneLightPoint& a, const ZoneLightPoint& b) {
      return a.pointOrder < b.pointOrder;
      });

  }


  for (auto& zone_light : zoneLightsWotlk)
  {
    auto& list = zoneLightPoints[zone_light.id];
    // insert reordered points
    for (auto& point : list)
    {
      zone_light.points.push_back(glm::vec2(point.posX, point.posY));
    }

    // calculate 2d extents
    math::aabb_2d const bounds (zone_light.points);

    zone_light._extents[0] = bounds.min;
    zone_light._extents[1] = bounds.max;
  }

}

Sky* Skies::findSkyById(int sky_id)
{
  for (auto& sky : skies)
  {
    if (sky.getId() == sky_id)
    {
      return &sky;
    }
  }
  return nullptr;
}

Skies::Skies(unsigned int mapid, Noggit::NoggitRenderContext context)
  : stars (ModelInstance("Environments\\Stars\\Stars.mdx", context))
  , _context(context)
  , _indices_count(0)
  , _last_pos(glm::vec3(0.0f, 0.0f, 0.0f))
{
  _map_id = static_cast<int>(mapid);

  bool has_global = false;
  for (DBCFile::Iterator i = gLightDB.begin(); i != gLightDB.end(); ++i)
  {
    if (mapid == i->getUInt(LightDB::Map))
    {
      Sky s(i, _context);
      skies.push_back(s);
      numSkies++;

      if (s.pos == glm::vec3(0, 0, 0))
        has_global = true;
    }
  }

  if (!has_global)
  {
    LogDebug << "No global light data found for the current map (id :" << mapid
        << ") using light id 1 as a fallback" << std::endl;
    for (DBCFile::Iterator i = gLightDB.begin(); i != gLightDB.end(); ++i)
    {
      if (1 == i->getUInt(LightDB::ID))
      {
        Sky s(i, _context);
        s.global = true;

        // Flagged, because this light does NOT belong to the loaded map. It is Light.dbc id 1,
        // Azeroth's global light, borrowed so the map is not pitch black. Its Sky::mapId() is
        // therefore 0 and not `mapid`, and saving it would write Azeroth's row -- rewriting the
        // global lighting of every zone on that continent from a different map's editor.
        // validateForSave refuses it by name; LightEditor offers to promote it instead.
        s.is_fallback_global = true;

        skies.push_back(s);
        numSkies++;
        break;
      }
    }
    using_fallback_global = true;
  }

  // sort skies from smallest to largest; global last.
  // smaller skies will have precedence when calculating weights to achieve smooth transitions etc.
  std::sort(skies.begin(), skies.end());

  // load Zone Lights data, was hardcoded in 3.3.5 and moved to a dbc in Cata. Noggit stores them in a csv
  if (Noggit::Project::CurrentProject::get()->projectVersion == Noggit::Project::ProjectVersion::WOTLK)
  {
    loadZoneLights(mapid);
  }
}

int Skies::mapId() const
{
  return _map_id;
}

int Skies::selectedLight() const
{
  return _selected_light_id;
}

void Skies::setSelectedLight(int light_id)
{
  _selected_light_id = 0;

  // Cleared on every light and set on at most one, in a single pass, because Sky::_selected is
  // read per-sky by the renderer and two lights flagged at once would draw two highlights for one
  // selection. Passing 0 is the documented way to clear.
  for (Sky& sky : skies)
  {
    bool const is_target = (light_id != 0 && sky.getId() == light_id);

    sky.setSelected(is_target);

    if (is_target)
    {
      _selected_light_id = light_id;
    }
  }
}

bool Skies::snapshotLight(int light_id, std::string const& name, Noggit::LightSnapshot& out) const
{
  Sky const* sky = nullptr;

  for (Sky const& candidate : skies)
  {
    if (candidate.getId() == light_id)
    {
      sky = &candidate;
      break;
    }
  }

  if (!sky)
  {
    return false;
  }

  out = Noggit::LightSnapshot();
  out.valid = true;
  out.light_id = sky->getId();
  out.map_id = sky->mapId();
  out.name = name;
  out.pos = sky->pos;
  out.r1 = sky->r1;
  out.r2 = sky->r2;

  for (int i = 0; i < NUM_SkyParamsNames; ++i)
  {
    out.param_ids[i] = sky->skyParams[i];
  }

  return true;
}

bool Skies::copyLightToClipboard(int light_id, std::string const& name)
{
  Noggit::LightSnapshot snapshot;

  if (!snapshotLight(light_id, name, snapshot))
  {
    return false;
  }

  Noggit::setLightClipboard(snapshot);

  return true;
}

Sky* Skies::pasteLight(Noggit::LightSnapshot const& snapshot, bool deep_copy_params, std::string& error)
{
  if (!snapshot.valid)
  {
    error = "The light clipboard is empty.";
    return nullptr;
  }

  // A light at the origin IS the map's global light -- that is the whole of the definition, in
  // Sky's constructor and in the client. So a paste at 0,0,0 is a request to create one, and the
  // only thing that can go wrong is that the map already has one of its own.
  bool const as_global = (snapshot.pos.x == 0.0f && snapshot.pos.y == 0.0f && snapshot.pos.z == 0.0f);

  if (as_global)
  {
    for (Sky const& sky : skies)
    {
      if (sky.global && !sky.is_fallback_global)
      {
        error = "Map " + std::to_string(_map_id) + " already has a global light (id "
              + std::to_string(sky.getId()) + "). A map has exactly one: the light at 0,0,0 that "
                "applies wherever no other light reaches.";
        return nullptr;
      }
    }
  }

  int new_light_id = gLightDB.getEmptyRecordID(LightDB::ID);

  if (new_light_id <= 0)
  {
    error = "Could not allocate a free Light.dbc id.";
    return nullptr;
  }

  // getEmptyRecordID answers from the DBC, and a paste writes nothing to the DBC until Save. So
  // two pastes in a row are otherwise handed the SAME id: the second Sky would shadow the first in
  // findSkyById, the panel would list two rows that resolve to one light, and saving the second
  // after the first would throw AlreadyExists. Stepping past the unsaved ids already in this map
  // is what makes paste, paste, paste, save work.
  while (findSkyById(new_light_id) != nullptr)
  {
    ++new_light_id;
  }

  Noggit::LightSnapshot pasted = snapshot;

  if (deep_copy_params)
  {
    // Params are allocated in one pass and BEFORE any Sky is built, so a failure half way through
    // leaves no Sky pointing at a param that was never created. It can leave the reverse -- a
    // SkyParam in the global map that nothing references -- which costs a few kilobytes, is never
    // written anywhere, and makes its id count as taken so a retry allocates a fresh one.
    // getEmptyRecordID is
    // only the starting point -- it scans the DBC for the maximum, and a paste writes nothing to
    // the DBC -- so param_id_is_free below is what actually decides each id.
    int const param_base = gLightParamsDB.getEmptyRecordID(LightParamsDB::ID);

    if (param_base <= 0)
    {
      error = "Could not allocate a free LightParams.dbc id.";
      return nullptr;
    }

    unsigned int next_param_id = static_cast<unsigned int>(param_base);

    // A candidate LightParams id is only usable if its 18 LightIntBand rows and its 6
    // LightFloatBand rows are ALSO free, because those row ids are derived from it arithmetically
    // rather than allocated. Same reason as the light id above: nothing is on disk until Save, so
    // the DBC alone cannot answer this for a second paste.
    auto const param_id_is_free = [] (unsigned int id) -> bool
    {
      if (id == 0 || skyparams::paramIdIsRegistered(id))
      {
        return false;
      }

      if (gLightParamsDB.CheckIfIdExists(id, LightParamsDB::ID))
      {
        return false;
      }

      if (id > static_cast<unsigned int>(std::numeric_limits<int>::max() / NUM_SkyColorNames))
      {
        return false;
      }

      int const int_first = lightIntBandFirstRow(static_cast<int>(id));
      int const float_first = lightFloatBandFirstRow(static_cast<int>(id));

      for (int i = 0; i < NUM_SkyColorNames; ++i)
      {
        if (gLightIntBandDB.CheckIfIdExists(static_cast<unsigned int>(int_first + i)))
        {
          return false;
        }
      }

      for (int i = 0; i < NUM_SkyFloatParamsNames; ++i)
      {
        if (gLightFloatBandDB.CheckIfIdExists(static_cast<unsigned int>(float_first + i)))
        {
          return false;
        }
      }

      return true;
    };

    // One new param per DISTINCT source param, so a light whose Clear and Storm slots already
    // share a param keeps sharing it after the paste instead of silently splitting into two.
    std::map<unsigned int, unsigned int> remapped;

    for (int i = 0; i < NUM_SkyParamsNames; ++i)
    {
      unsigned int const source = snapshot.param_ids[i];

      if (!source)
      {
        pasted.param_ids[i] = 0;
        continue;
      }

      auto const existing = remapped.find(source);

      if (existing != remapped.end())
      {
        pasted.param_ids[i] = existing->second;
        continue;
      }

      while (!param_id_is_free(next_param_id))
      {
        ++next_param_id;

        if (next_param_id == 0) // wrapped, which means there is no free id at all
        {
          error = "Could not allocate a free LightParams.dbc id with free band rows.";
          return nullptr;
        }
      }

      unsigned int const target = next_param_id++;

      if (!skyparams::createParamCopy(source, target, _context))
      {
        error = "Could not read LightParams.dbc row " + std::to_string(source)
              + ", so the deep copy was abandoned before anything was created.";
        return nullptr;
      }

      remapped[source] = target;
      pasted.param_ids[i] = target;
    }
  }

  // The borrowed global light goes first, and only once the new one is certain to be created. It
  // is Light.dbc id 1 belonging to map 0, shown here only because this map had no global light of
  // its own; leaving it in the list next to a real one would give the map two global lights on
  // screen, one of which cannot be saved and the other of which now supersedes it.
  if (as_global)
  {
    for (std::size_t i = 0; i < skies.size(); ++i)
    {
      if (skies[i].is_fallback_global)
      {
        skies.erase(skies.begin() + static_cast<std::ptrdiff_t>(i));
        numSkies--;
        break;
      }
    }

    using_fallback_global = false;
  }

  Sky new_sky(pasted, new_light_id, _map_id, _context);

  skies.push_back(new_sky);
  numSkies++;

  std::sort(skies.begin(), skies.end());
  force_update();

  return findSkyById(new_light_id);
}

bool Skies::deleteSky(int light_id, std::string& error)
{
  Sky const* sky = findSkyById(light_id);

  if (!sky)
  {
    error = "That light is not loaded for this map.";
    return false;
  }

  if (sky->is_fallback_global)
  {
    error = "That is Azeroth's global light, borrowed because this map has none of its own. "
            "Deleting it would remove light 1 from Light.dbc and darken every zone that uses it.";
    return false;
  }

  if (sky->global)
  {
    error = "A map cannot be left without a global light -- it is what lights everywhere outside "
            "every other light's radius.";
    return false;
  }

  bool const was_written = !sky->is_new_record;

  if (was_written)
  {
    try
    {
      gLightDB.removeRecord(static_cast<std::size_t>(light_id), LightDB::ID);
    }
    catch (...)
    {
      error = "Light.dbc has no row " + std::to_string(light_id) + " to remove.";
      return false;
    }

    // Only Light.dbc is saved. The LightParams / LightIntBand / LightFloatBand rows this light
    // pointed at are deliberately left in place: params are shared -- the editor panel counts the
    // users of one -- and cascading a delete through them would recolour unrelated zones,
    // including on other maps entirely. A param row no Light.dbc row references is unreachable by
    // the client, so leaving it costs 4 bytes a field and nothing else.
    gLightDB.save();
  }

  for (std::size_t i = 0; i < skies.size(); ++i)
  {
    if (skies[i].getId() == light_id)
    {
      skies.erase(skies.begin() + static_cast<std::ptrdiff_t>(i));
      numSkies--;
      break;
    }
  }

  if (_selected_light_id == light_id)
  {
    _selected_light_id = 0;
  }

  std::sort(skies.begin(), skies.end());
  force_update();

  return true;
}

// returns the global light, not the highest weight
Sky* Skies::findSkyWeights(glm::vec3 pos)
{
  Sky* default_sky = nullptr;

  for (auto& sky : skies)
  {
    if (sky.pos == glm::vec3(0, 0, 0))
    {
      default_sky = &sky;
      break;
    }
  }

  std::sort(skies.begin(), skies.end(), [=](Sky& a, Sky& b)
  {
    return glm::distance(pos, a.pos) > glm::distance(pos, b.pos);
  });

  for (auto& sky : skies)
  {
    float distance_to_light = glm::distance(pos, sky.pos);

    if (default_sky == &sky || distance_to_light > sky.r2)
    {
      sky.weight = 0.f;
      continue;
    }

    float length_of_falloff = sky.r2 - sky.r1;
    sky.weight = (sky.r2 - distance_to_light) / length_of_falloff;

    if (distance_to_light <= sky.r1)
    {
      sky.weight = 1.0f;
    }

  }

  // Light zones
  glm::vec2 const pos_2d = glm::vec2(pos.x, pos.z);
  for (auto& lightzone : zoneLightsWotlk)
  {
    if (math::is_inside_of_aabb_2d(pos_2d, lightzone._extents[0], lightzone._extents[1]))
    {
      bool inside = math::is_inside_of_polygon(pos_2d, lightzone.points);

      if (inside)
      {
        Sky* sky = findSkyById(lightzone.lightId);
        if (sky)
          sky->weight = 1.0f;
      }
    }
  }

  return default_sky;
}

Sky* Skies::findClosestSkyByWeight()
{
    // gets the highest weight sky
    if (skies.size() == 0)
        return nullptr;

    Sky* closest_sky = &skies[0];
    for (auto& sky : skies)
    {
        // use >= to make sure when we have multiple with the same weight, 
        // last one has priority, because it is the closest
        // skies is sorted by distance to center
        if (sky.weight > 0.0f && sky.weight >= closest_sky->weight)
            closest_sky = &sky;
    }
    return closest_sky;
}

Sky* Skies::findClosestSkyByDistance(glm::vec3 pos)
{
    if (skies.size() == 0)
        return nullptr;

    Sky* closest = &skies[0];
    float distance = 1000000.f;
    for (auto& sky : skies)
    {
        float distanceToCenter = glm::distance(pos, sky.pos);

        if (distanceToCenter <= sky.r2 && distanceToCenter < distance)
        {
            distance = distanceToCenter;
            closest = &sky;
        }
    }

    return closest;
}

void Skies::setCurrentParam(int param_id)
{
  assert(param_id < NUM_SkyParamsNames);

  for (auto& sky : skies)
  {
      Sky* skyptr = &sky;
      skyptr->curr_sky_param = param_id;
  }
}

void Skies::update_sky_colors(glm::vec3 pos, int time, bool global_only)
{
  if (numSkies == 0 || (_last_time == time && _last_pos == pos && !_force_update))
  {
    return;
  }
  _force_update = false;

  Sky* default_sky = findSkyWeights(pos);

  // initialize lightning with default(global) light
  if (default_sky && default_sky->getCurrentParam().has_value())
  {
    for (int i = 0; i < NUM_SkyColorNames; ++i)
    {
      color_set[i] = default_sky->colorFor(i, time);
    }

    // float values
    float fog_distance = default_sky->floatParamFor(SKY_FOG_DISTANCE, time);
    _fog_distance = fog_distance == 0.0f ? 6500.0f : fog_distance;

    float fog_multiplier = default_sky->floatParamFor(SKY_FOG_MULTIPLIER, time);
    _fog_multiplier = fog_multiplier == 0.0f ? 0.1f : fog_multiplier;

    _celestial_glow = default_sky->floatParamFor(SKY_CELESTIAL_GLOW, time);
    _cloud_density = default_sky->floatParamFor(SKY_CLOUD_DENSITY, time);
    _unknown_float_param4 = default_sky->floatParamFor(SKY_UNK_FLOAT_PARAM_4, time);
    _unknown_float_param5 = default_sky->floatParamFor(SKY_UNK_FLOAT_PARAM_5, time);

    // param values
    auto param_opt = default_sky->getCurrentParam();
    if (param_opt.has_value())
    {
      SkyParam* const default_sky_param = param_opt.value();

      _river_shallow_alpha = default_sky_param->river_shallow_alpha();
      _river_deep_alpha = default_sky_param->river_deep_alpha();
      _ocean_shallow_alpha = default_sky_param->ocean_shallow_alpha();
      _ocean_deep_alpha = default_sky_param->ocean_deep_alpha();
      _glow = default_sky_param->glow();
    }
    else
    {
      // if no param data, use some default. TODO : check how client does it.
      _river_shallow_alpha = 0.5f;
      _river_deep_alpha = 1.0f;
      _ocean_shallow_alpha = 0.75f;
      _ocean_deep_alpha = 1.0f;
      _glow = 0.5f;
    }
  }
  else
  {
    LogError << "Failed to load default light. Something went seriously wrong. Potentially corrupt Light.dbc" << std::endl;

    for (int i = 0; i < NUM_SkyColorNames; ++i)
    {
      color_set[i] = glm::vec3(1.0f, 1.0f, 1.0f);
    }

    _fog_multiplier = 0.1f;
    _fog_distance = 6500.0f;
    _celestial_glow = 1.0f;
    _cloud_density = 1.0f;
    _unknown_float_param4 = 1.0f;
    _unknown_float_param5 = 1.0f;

    _river_shallow_alpha = 0.5f;
    _river_deep_alpha = 1.0f;
    _ocean_shallow_alpha = 0.75f;
    _ocean_deep_alpha = 1.0f;
    _glow = 0.5f;

  }

  if (!global_only)
  {
    // Blending interpolation with local lights
    for (size_t j = 0; j<skies.size(); j++) 
    {
      Sky const& sky = skies[j];

      if (sky.weight > 0.f)
      {
        // now calculate the color rows
        for (int i = 0; i < NUM_SkyColorNames; ++i) 
        {
          if ((sky.colorFor(i, time).x>1.0f) || (sky.colorFor(i, time).y>1.0f) || (sky.colorFor(i, time).z>1.0f))
          {
            LogDebug << "Sky " << j << " " << i << " is out of bounds!" << std::endl;
            continue;
          }
          auto timed_color = sky.colorFor(i, time);
          color_set[i] = glm::mix(color_set[i], timed_color, sky.weight);
        }

        auto param_opt = sky.getCurrentParam();
        if (param_opt.has_value())
        {
          SkyParam* default_sky_param = param_opt.value();

          float sky_weight_remain = (1.0f - sky.weight);

          float fog_distance = sky.floatParamFor(SKY_FOG_DISTANCE, time);
          if (fog_distance != 0.0f)
            _fog_distance = (_fog_distance * sky_weight_remain) + (fog_distance * sky.weight);

          float fog_multiplier = sky.floatParamFor(SKY_FOG_MULTIPLIER, time);
          if (fog_multiplier != 0.0f)
            _fog_multiplier = (_fog_multiplier * sky_weight_remain) + (fog_multiplier * sky.weight);

          _celestial_glow = (_celestial_glow * sky_weight_remain) + (sky.floatParamFor(SKY_CELESTIAL_GLOW, time) * sky.weight);
          _cloud_density = (_cloud_density * sky_weight_remain) + (sky.floatParamFor(SKY_CLOUD_DENSITY, time) * sky.weight);
          _unknown_float_param4 = (_unknown_float_param4 * sky_weight_remain) + (sky.floatParamFor(SKY_UNK_FLOAT_PARAM_4, time) * sky.weight);
          _unknown_float_param5 = (_unknown_float_param5 * sky_weight_remain) + (sky.floatParamFor(SKY_UNK_FLOAT_PARAM_5, time) * sky.weight);

          _river_shallow_alpha = (_river_shallow_alpha * sky_weight_remain) + (default_sky_param->river_shallow_alpha() * sky.weight);
          _river_deep_alpha = (_river_deep_alpha * sky_weight_remain) + (default_sky_param->river_deep_alpha() * sky.weight);
          _ocean_shallow_alpha = (_ocean_shallow_alpha * sky_weight_remain) + (default_sky_param->ocean_shallow_alpha() * sky.weight);
          _ocean_deep_alpha = (_ocean_deep_alpha * sky_weight_remain) + (default_sky_param->ocean_deep_alpha() * sky.weight);

          _glow = (_glow * sky_weight_remain) + (default_sky_param->glow() * sky.weight);
        }
        else
        {
          // if no data for param index, it just uses default values from global light, no blending
        }

      }

    }
  }

  const float fogEnd = _fog_distance / 36.f;
  const float fogStart = _fog_multiplier * fogEnd;
  const float fogRange = fogEnd - fogStart;

  // constexpr float fogFarClip = 500.f; // Max fog farclip possible
  constexpr float fogFarClip = 1583.333374f; // 1583.333374 for wrath/tbc zones, 791.666687 for vanilla zones

  if (fogRange <= fogFarClip)
  {
    _fog_rate = ((1.0f - (fogRange / fogFarClip)) * 5.5f) + 1.5f;
  } else
  {
    _fog_rate = 1.5f;
  }

  _last_pos = pos;
  _last_time = time;

  _need_color_buffer_update = true;  
}

bool Skies::draw(glm::mat4x4 const& model_view
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
                )
{
  if (numSkies == 0)
  {
    return false;
  }

  if (!_uploaded)
  {
    upload();
  }

  if (_need_color_buffer_update)
  {
    update_color_buffer();
  }

  {
    OpenGL::Scoped::use_program shader {*_program.get()};

    if(_need_vao_update)
    {
      update_vao(shader);
    }

    {
      OpenGL::Scoped::vao_binder const _ (_vao);
       
      shader.uniform("model_view_projection", projection * model_view);
      shader.uniform("camera_pos", glm::vec3(camera_pos.x, camera_pos.y, camera_pos.z));

      gl.drawElements(GL_TRIANGLES, _indices_count, GL_UNSIGNED_SHORT, nullptr);
    }
  }

  if (draw_skybox)
  {
    bool combine_flag = false;
    bool has_skybox = false;

    // only draw one skybox model ?
    for (Sky& sky : skies)
    {
      auto param_opt = sky.getCurrentParam();
      if (sky.weight > 0.f && param_opt.has_value() && param_opt.value()->skybox)
      {
        has_skybox = true;

        SkyParam* curr_param = param_opt.value();

        if ((curr_param->skyboxFlags & LIGHT_SKYBOX_COMBINE))
            combine_flag = true; // flag 0x2 = still render stars, sun and moons and clouds
    
        auto& model = curr_param->skybox.value();
        model.model->trans = sky.weight;
        model.pos = camera_pos;
        model.scale = 0.1f;
        model.recalcExtents();
    
        OpenGL::M2RenderState model_render_state;
        model_render_state.tex_arrays = {0, 0};
        model_render_state.tex_indices = {0, 0};
        model_render_state.tex_unit_lookups = {-1, -1};
        gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        gl.disable(GL_BLEND);
        gl.depthMask(GL_TRUE);
        m2_shader.uniform("blend_mode", 0);
        m2_shader.uniform("unfogged", static_cast<int>(model_render_state.unfogged));
        m2_shader.uniform("unlit",  static_cast<int>(model_render_state.unlit));
        m2_shader.uniform("tex_unit_lookup_1", 0);
        m2_shader.uniform("tex_unit_lookup_2", 0);
        m2_shader.uniform("pixel_shader", 0);

        int skyboxtime = animtime;
        if ((curr_param->skyboxFlags & LIGHT_SKYBOX_FULL_DAY))
        {
          unsigned int anim_lenght = model.model->get_anim_lenght(0);

          // Calculate the normalized time within the day (0.0 to 1.0)
          float day_fraction = static_cast<float>(time % DAY_DURATION) / DAY_DURATION;

          // animation time from day time %
          skyboxtime = static_cast<int>(day_fraction * anim_lenght);
        }
    
        model.model->renderer()->draw(model_view
                                     , model
                                     , m2_shader
                                     , model_render_state
                                     , frustum
                                     , 1000000
                                     , camera_pos
                                     , skyboxtime
                                     , display_mode::in_3D
                                     , true
                                     , true);
      }
    }
    // if it's night, draw the stars
    if (light_stats.nightIntensity > 0 && (combine_flag || !has_skybox))
    {
      stars.model->trans = light_stats.nightIntensity;
      stars.pos = camera_pos;
      stars.scale = 0.1f;
      stars.recalcExtents();
    
      OpenGL::M2RenderState model_render_state;
      model_render_state.tex_arrays = {0, 0};
      model_render_state.tex_indices = {0, 0};
      model_render_state.tex_unit_lookups = {-1, -1};
      gl.blendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      gl.disable(GL_BLEND);
      gl.depthMask(GL_TRUE);
      m2_shader.uniform("blend_mode", 0);
      m2_shader.uniform("unfogged", static_cast<int>(model_render_state.unfogged));
      m2_shader.uniform("unlit",  static_cast<int>(model_render_state.unlit));
      m2_shader.uniform("tex_unit_lookup_1", 0);
      m2_shader.uniform("tex_unit_lookup_2", 0);
      m2_shader.uniform("pixel_shader", 0);
    
      stars.model->renderer()->draw(model_view
                                   , stars
                                   , m2_shader
                                   , model_render_state
                                   , frustum
                                   , 1000000
                                   , camera_pos
                                   , animtime
                                   , display_mode::in_3D
                                   , true
                                   , true);
    }
  }


  return true;
}

bool Skies::hasSkies() const
{
  return numSkies > 0;
}

float Skies::river_shallow_alpha() const
{
  return _river_shallow_alpha;
}

float Skies::river_deep_alpha() const
{
  return _river_deep_alpha;
}

float Skies::ocean_shallow_alpha() const
{
  return _ocean_shallow_alpha;
}

float Skies::ocean_deep_alpha() const
{
  return _ocean_deep_alpha;
}

float Skies::fog_distance_end() const
{
  return _fog_distance / 36.f;
}

float Skies::fog_distance_start() const
{
  return (_fog_distance / 36.f) * _fog_multiplier;
}

float Skies::fog_distance_multiplier() const
{
  return _fog_multiplier;
}

float Skies::celestial_glow() const
{
  return _celestial_glow;
}

float Skies::cloud_density() const
{
  return _cloud_density;
}

float Skies::unknown_float_param4() const
{
  return _unknown_float_param4;
}

float Skies::unknown_float_param5() const
{
  return _unknown_float_param5;
}

float Skies::glow() const
{
  return _glow;
}

float Skies::fogRate() const
{
  return _fog_rate;
}

void Skies::unload()
{
  _program.reset();
  _vertex_array.unload();
  _buffers.unload();
  _sphere_render.unload();

  _uploaded = false;
  _need_vao_update = true;

}

void Skies::force_update()
{
  _force_update = true;
}

void Skies::upload()
{
  _program.reset(new OpenGL::program(
    {
        {GL_VERTEX_SHADER, R"code(
#version 330 core

uniform mat4 model_view_projection;
uniform vec3 camera_pos;

in vec3 position;
in vec3 color;

out vec3 f_color;

void main()
{
  vec4 pos = vec4(position + camera_pos, 1.f);
  gl_Position = model_view_projection * pos;
  f_color = color;
}
)code" }
        , {GL_FRAGMENT_SHADER, R"code(
#version 330 core

in vec3 f_color;

out vec4 out_color;

void main()
{
  out_color = vec4(f_color, 1.);
}
)code" }
    }
  ));

  _vertex_array.upload();
  _buffers.upload();

  std::vector<glm::vec3> vertices;
  std::vector<std::uint16_t> indices;

  glm::vec3 basepos1[cnum], basepos2[cnum];

  for (int h = 0; h < hseg; h++)
  {
    for (int i = 0; i < cnum; ++i)
    {
      basepos1[i] = basepos2[i] = glm::vec3(glm::cos(math::radians(angles[i])._) * rad, glm::sin(math::radians(angles[i])._)*rad, 0);

      math::rotate(0, 0, &basepos1[i].x, &basepos1[i].z, math::radians(glm::pi<float>() *2.0f / hseg * h));
      math::rotate(0, 0, &basepos2[i].x, &basepos2[i].z, math::radians(glm::pi<float>() *2.0f / hseg * (h + 1)));
    }

    for (int v = 0; v < cnum - 1; v++)
    {
      int start = static_cast<int>(vertices.size());

      vertices.push_back(basepos2[v]);
      vertices.push_back(basepos1[v]);
      vertices.push_back(basepos1[v + 1]);
      vertices.push_back(basepos2[v + 1]);

      indices.push_back(start+0);
      indices.push_back(start+1);
      indices.push_back(start+2);

      indices.push_back(start+2);
      indices.push_back(start+3);
      indices.push_back(start+0);
    }
  }

  gl.bufferData<GL_ARRAY_BUFFER, glm::vec3>(_vertices_vbo, vertices, GL_STATIC_DRAW);
  gl.bufferData<GL_ELEMENT_ARRAY_BUFFER, std::uint16_t>(_indices_vbo, indices, GL_STATIC_DRAW);

  _indices_count = static_cast<int>(indices.size());

  _uploaded = true;
  _need_vao_update = true;
}

void Skies::update_vao(OpenGL::Scoped::use_program& shader)
{
  OpenGL::Scoped::index_buffer_manual_binder indices_binder (_indices_vbo);

  {
    OpenGL::Scoped::vao_binder const _ (_vao);

    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> vertices_buffer (_vertices_vbo);
    shader.attrib("position", 3, GL_FLOAT, GL_FALSE, 0, 0);

    OpenGL::Scoped::buffer_binder<GL_ARRAY_BUFFER> colors_buffer (_colors_vbo);
    shader.attrib("color", 3, GL_FLOAT, GL_FALSE, 0, 0);

    indices_binder.bind();
  }

  _need_vao_update = false;
}

void Skies::update_color_buffer()
{
  std::vector<glm::vec3> colors;

  for (int h = 0; h < hseg; h++)
  {
    for (int v = 0; v < cnum - 1; v++)
    {
      colors.push_back(color_set[skycolors[v]]);
      colors.push_back(color_set[skycolors[v]]);
      colors.push_back(color_set[skycolors[v + 1]]);
      colors.push_back(color_set[skycolors[v + 1]]);
    }
  }

  gl.bufferData<GL_ARRAY_BUFFER, glm::vec3>(_colors_vbo, colors, GL_STATIC_DRAW);

  _need_vao_update = true;
}


void OutdoorLightStats::interpolate(OutdoorLightStats *a, OutdoorLightStats *b, float r)
{
  static constexpr unsigned DayNight_SecondsPerDay = 86400;

  float progressDayAndNight = r / DayNight_SecondsPerDay;

  float phiValue = 0;
  const float thetaValue = 3.926991f;
  const float phiTable[4] =
    {
      2.2165682f,
      1.9198623f,
      2.2165682f,
      1.9198623f
    };

  unsigned currentPhiIndex = static_cast<unsigned>(progressDayAndNight / 0.25f);
  unsigned nextPhiIndex = 0;

  if (currentPhiIndex < 3)
    nextPhiIndex = currentPhiIndex + 1;

  // Lerp between the current value of phi and the next value of phi
  {
    float transitionProgress = (progressDayAndNight / 0.25f) - currentPhiIndex;

    float currentPhiValue = phiTable[currentPhiIndex];
    float nextPhiValue = phiTable[nextPhiIndex];

    phiValue = glm::mix(currentPhiValue, nextPhiValue, transitionProgress);
  }

  // Convert from Spherical Position to Cartesian coordinates
  float sinPhi = glm::sin(phiValue);
  float cosPhi = glm::cos(phiValue);

  float sinTheta = glm::sin(thetaValue);
  float cosTheta = glm::cos(thetaValue);

  dayDir.x = sinPhi * cosTheta;
  dayDir.y = sinPhi * sinTheta;
  dayDir.z = cosPhi;

  float ir = 1.0f - progressDayAndNight;
  nightIntensity = a->nightIntensity * ir + b->nightIntensity * progressDayAndNight;
}

OutdoorLighting::OutdoorLighting()
{

  static constexpr std::array<int, 24> night_hours =
    {1, 1, 1, 1, 1, 1,
     0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 0, 0,
     0, 0, 0, 0, 1, 1};

  for (int i = 0; i < 24; ++i)
  {
    OutdoorLightStats ols;
    ols.nightIntensity = night_hours[i];
    lightStats.push_back(ols);
  }
}

OutdoorLightStats OutdoorLighting::getLightStats(int time)
{
  // ASSUME: only 24 light info records, one for each whole hour
  //! \todo  generalize this if the data file changes in the future

  int normalized_time ((static_cast<int>(time) % DAY_DURATION) / 2);

  static constexpr unsigned DayNight_SecondsPerDay = 86400;

  long progressDayAndNight = (static_cast<float>(normalized_time) * 120);

  while (progressDayAndNight < 0 || progressDayAndNight > DayNight_SecondsPerDay)
  {
    if (progressDayAndNight > DayNight_SecondsPerDay)
      progressDayAndNight -= DayNight_SecondsPerDay;

    if (progressDayAndNight < 0)
      progressDayAndNight += DayNight_SecondsPerDay;
  }

  OutdoorLightStats out;

  OutdoorLightStats *a, *b;
  int ta = normalized_time / 60;
  int tb = (ta + 1) % 24;

  a = &lightStats[ta];
  b = &lightStats[tb];

  out.interpolate(a, b, progressDayAndNight);

  return out;
}

bool Sky::operator<(const Sky& s) const
{
  if (global) return false;
  else if (s.global) return true;
  else return r2 < s.r2;
}

bool Sky::selected() const
{
  return _selected;
}

namespace
{
  // Fetch a row for writing, adding it when it is not there.
  //
  // The old writer chose addRecord vs getByID from SkyParam::_is_new_param_record alone, so a band
  // row missing from an otherwise existing param -- which SkyParam's constructor tolerates
  // silently, it catches NotFound per row and logs -- threw NotFound into a catch(...) that logged
  // and moved on, leaving that one colour channel unwritten while the other 17 were saved. Asking
  // the file instead cannot get that wrong.
  DBCFile::Record recordForWriting(DBCFile& db, int id)
  {
    if (db.CheckIfIdExists(static_cast<unsigned int>(id)))
    {
      return db.getByID(static_cast<unsigned int>(id));
    }

    return db.addRecord(static_cast<std::size_t>(id));
  }

  // The Light.dbc weather slot names, in DataIDs order, so an error message can name the slot the
  // user sees in the combo box rather than an index.
  char const* skyParamSlotName(int slot)
  {
    switch (slot)
    {
      case SKY_PARAM_CLEAR:             return "Clear Weather";
      case SKY_PARAM_CLEAR_UNDERWATER:  return "Clear Weather Underwater";
      case SKY_PARAM_TORM:              return "Storm Weather";
      case SKY_PARAM_STORM_UNDERWATER:  return "Storm Weather Underwater";
      case SKY_PARAM_DEATH:             return "Death Effect";
      default:                          return "unknown param";
    }
  }

  // The furthest a light may sit from the origin. Twice the 17066.66656 half-extent of the 64x64
  // ADT grid, which is the range the editor's own position spin boxes already allow. Anything
  // beyond it is not a place, it is a float that went wrong.
  float constexpr MAX_LIGHT_COORDINATE = 17066.66656f * 2.0f;

  // LightIntBand and LightFloatBand each hold sixteen time/value pairs plus a count. Writing a
  // count larger than sixteen produces a record that tells the client to read past the end of its
  // own row -- the most damaging thing a light save can do, and the one the old writer had no
  // guard against at all.
  int constexpr MAX_BAND_ENTRIES = 16;
}

bool Sky::validateForSave(std::string& reason) const
{
  if (is_fallback_global)
  {
    // Deliberately does not name the loaded map: this Sky carries the map it CAME FROM (0), and
    // a message that said "map 0 has no global light" would be exactly backwards.
    reason = "This is Light.dbc row " + std::to_string(Id) + ", which belongs to map "
           + std::to_string(_map_id) + ". It only appears in this map's list because this map has "
             "no global light of its own and Noggit borrowed one. Saving it would rewrite the "
             "lighting of every zone on the map it really belongs to. Use \"Give this map its own "
             "global light\" instead.";
    return false;
  }

  if (Id <= 0)
  {
    reason = "This light has no valid Light.dbc id.";
    return false;
  }

  if (_map_id < 0)
  {
    reason = "This light has no valid map id.";
    return false;
  }

  bool const row_exists = gLightDB.CheckIfIdExists(static_cast<unsigned int>(Id), LightDB::ID);

  // The two ways this can be wrong are opposite and both destructive. A new light whose id is
  // already taken makes addRecord throw AlreadyExists, which the old code caught at the very
  // bottom of the function -- after LightSkybox.dbc had already been saved to disk from inside the
  // loop. An existing light whose row has since gone makes getByID throw NotFound and loses the
  // edit entirely.
  if (is_new_record && row_exists)
  {
    reason = "Light.dbc already has a row " + std::to_string(Id)
           + ", so this new light cannot be added under that id.";
    return false;
  }

  if (!is_new_record && !row_exists)
  {
    reason = "Light.dbc no longer has a row " + std::to_string(Id) + " to update.";
    return false;
  }

  if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z))
  {
    reason = "This light's position is not a finite number.";
    return false;
  }

  if ( std::fabs(pos.x) > MAX_LIGHT_COORDINATE
    || std::fabs(pos.y) > MAX_LIGHT_COORDINATE
    || std::fabs(pos.z) > MAX_LIGHT_COORDINATE
     )
  {
    reason = "This light sits outside the map grid.";
    return false;
  }

  if (!std::isfinite(r1) || !std::isfinite(r2))
  {
    reason = "This light's radii are not finite numbers.";
    return false;
  }

  if (r1 < 0.0f || r2 < 0.0f)
  {
    reason = "A light radius cannot be negative.";
    return false;
  }

  if (r2 < r1)
  {
    reason = "The outer radius (" + std::to_string(r2) + ") is smaller than the inner radius ("
           + std::to_string(r1) + "). The band between them is what blends this light with the "
             "global one, and it is evaluated as (outer - distance) / (outer - inner).";
    return false;
  }

  int populated_slots = 0;

  for (int slot = 0; slot < NUM_SkyParamsNames; ++slot)
  {
    int const param_id = static_cast<int>(skyParams[slot]);

    if (param_id == 0)
    {
      continue; // an unused weather slot is normal; only the first five are ever filled in 3.3.5
    }

    ++populated_slots;

    // Overflow guard on the band-row arithmetic. param_id * 18 has to stay a positive int, or the
    // derived row id wraps and the write lands on whatever row that resolves to.
    if (param_id > (std::numeric_limits<int>::max() / NUM_SkyColorNames))
    {
      reason = std::string("LightParams id ") + std::to_string(param_id) + " in the "
             + skyParamSlotName(slot) + " slot is too large to address its LightIntBand rows.";
      return false;
    }

    auto const param_opt = getParam(slot);

    if (!param_opt.has_value() || param_opt.value() == nullptr)
    {
      reason = std::string("The ") + skyParamSlotName(slot) + " slot points at LightParams row "
             + std::to_string(param_id) + ", which cannot be read.";
      return false;
    }

    SkyParam const* const param = param_opt.value();

    if (param->Id != param_id)
    {
      reason = std::string("The ") + skyParamSlotName(slot) + " slot points at LightParams row "
             + std::to_string(param_id) + " but resolved to row " + std::to_string(param->Id) + ".";
      return false;
    }

    bool const param_row_exists
      (gLightParamsDB.CheckIfIdExists(static_cast<unsigned int>(param_id), LightParamsDB::ID));

    if (param->_is_new_param_record && param_row_exists)
    {
      reason = "LightParams.dbc already has a row " + std::to_string(param_id)
             + ", so the new parameters for the " + skyParamSlotName(slot)
             + " slot cannot be added under that id.";
      return false;
    }

    if (!param->_is_new_param_record && !param_row_exists)
    {
      reason = "LightParams.dbc has no row " + std::to_string(param_id) + " for the "
             + skyParamSlotName(slot) + " slot.";
      return false;
    }

    // A brand new param owns 18 colour rows and 6 float rows that must all be free, because
    // addRecord throws AlreadyExists on a collision. The old writer swallowed that exception per
    // row, which is exactly how a param ends up with some of its colours written and the rest
    // still holding another param's data.
    if (param->_is_new_param_record)
    {
      int const int_first = lightIntBandFirstRow(param_id);
      int const float_first = lightFloatBandFirstRow(param_id);

      for (int i = 0; i < NUM_SkyColorNames; ++i)
      {
        if (gLightIntBandDB.CheckIfIdExists(static_cast<unsigned int>(int_first + i)))
        {
          reason = "LightIntBand.dbc row " + std::to_string(int_first + i)
                 + " is already taken, so LightParams " + std::to_string(param_id)
                 + " cannot own it.";
          return false;
        }
      }

      for (int i = 0; i < NUM_SkyFloatParamsNames; ++i)
      {
        if (gLightFloatBandDB.CheckIfIdExists(static_cast<unsigned int>(float_first + i)))
        {
          reason = "LightFloatBand.dbc row " + std::to_string(float_first + i)
                 + " is already taken, so LightParams " + std::to_string(param_id)
                 + " cannot own it.";
          return false;
        }
      }
    }

    for (int i = 0; i < NUM_SkyColorNames; ++i)
    {
      if (static_cast<int>(param->colorRows[i].size()) > MAX_BAND_ENTRIES)
      {
        reason = "Colour band " + std::to_string(i) + " of LightParams " + std::to_string(param_id)
               + " has " + std::to_string(param->colorRows[i].size())
               + " time steps. A LightIntBand row holds 16.";
        return false;
      }
    }

    for (int i = 0; i < NUM_SkyFloatParamsNames; ++i)
    {
      if (static_cast<int>(param->floatParams[i].size()) > MAX_BAND_ENTRIES)
      {
        reason = "Float band " + std::to_string(i) + " of LightParams " + std::to_string(param_id)
               + " has " + std::to_string(param->floatParams[i].size())
               + " time steps. A LightFloatBand row holds 16.";
        return false;
      }

      for (auto const& entry : param->floatParams[i])
      {
        if (!std::isfinite(entry.value))
        {
          reason = "Float band " + std::to_string(i) + " of LightParams " + std::to_string(param_id)
                 + " contains a value that is not a finite number. Fog distance and fog multiplier "
                   "come out of this table, and a NaN there renders the map empty.";
          return false;
        }
      }
    }
  }

  if (populated_slots == 0)
  {
    reason = "This light references no LightParams rows at all, so the client would have no "
             "colours to read for it.";
    return false;
  }

  return true;
}

bool Sky::save_to_dbc()
{
  std::string reason;

  // Everything is decided before a single record is touched. A light spans five linked DBCs and a
  // half-written set is a corrupt lighting table for the whole map, not only for the zone being
  // edited -- so the only safe shape for this function is validate, then write, and never
  // partially do either.
  if (!validateForSave(reason))
  {
    LogError << "Refusing to save light " << Id << ": " << reason << std::endl;
    return false;
  }

  try
  {
    // The distinct params this light references, each listed once.
    //
    // Once, because two weather slots routinely share a LightParams id -- writing that param's 24
    // band rows twice is wasted work, and for a NEW param the second pass would hit AlreadyExists
    // on rows the first pass had just added.
    struct ParamWrite
    {
      int slot;
      SkyParam* param;
      int skybox_id;
    };

    std::vector<ParamWrite> writes;
    std::set<int> seen_param_ids;

    for (int slot = 0; slot < NUM_SkyParamsNames; ++slot)
    {
      if (!skyParams[slot])
        continue;

      auto const param_opt = getParam(slot);

      // validateForSave already proved both of these; they stay because getParam caches and a
      // future caller could reach this without validating first.
      if (!param_opt.has_value() || param_opt.value() == nullptr)
        continue;

      SkyParam* const param = param_opt.value();

      if (!seen_param_ids.insert(param->Id).second)
        continue;

      writes.push_back({slot, param, 0});
    }

    bool wrote_skybox_dbc = false;
    bool wrote_param_dbc = false;
    bool wrote_colors_dbc = false;
    bool wrote_floats_dbc = false;

    // ---- LightSkybox.dbc first, because LightParams.skybox is a reference into it -------------
    for (ParamWrite& write_entry : writes)
    {
      SkyParam const* const param = write_entry.param;

      if (!param->skybox.has_value())
      {
        write_entry.skybox_id = 0;
        continue;
      }

      std::string const filepath (param->skybox.value().model->file_key().filepath());

      for (DBCFile::Iterator i = gLightSkyboxDB.begin(); i != gLightSkyboxDB.end(); ++i)
      {
        if (i->getString(LightSkyboxDB::filename) == filepath
         && i->getInt(LightSkyboxDB::flags) == param->skyboxFlags)
        {
          write_entry.skybox_id = i->getInt(LightSkyboxDB::ID);
          break;
        }
      }

      if (write_entry.skybox_id == 0)
      {
        int const new_skybox_id = gLightSkyboxDB.getEmptyRecordID();

        DBCFile::Record record = gLightSkyboxDB.addRecord(static_cast<std::size_t>(new_skybox_id));
        record.writeString(LightSkyboxDB::filename, filepath);
        record.write(LightSkyboxDB::flags, param->skyboxFlags);

        write_entry.skybox_id = new_skybox_id;
        wrote_skybox_dbc = true;
      }
    }

    // ---- LightParams.dbc ---------------------------------------------------------------------
    for (ParamWrite const& write_entry : writes)
    {
      SkyParam* const param = write_entry.param;

      if (!param->_need_save && !param->_is_new_param_record)
        continue;

      DBCFile::Record record = recordForWriting(gLightParamsDB, param->Id);

      record.write(LightParamsDB::highlightSky, int(param->highlight_sky()));
      record.write(LightParamsDB::water_shallow_alpha, param->river_shallow_alpha());
      record.write(LightParamsDB::water_deep_alpha, param->river_deep_alpha());
      record.write(LightParamsDB::ocean_shallow_alpha, param->ocean_shallow_alpha());
      record.write(LightParamsDB::ocean_deep_alpha, param->ocean_deep_alpha());
      record.write(LightParamsDB::glow, param->glow());
      record.write(LightParamsDB::skybox, write_entry.skybox_id);

      wrote_param_dbc = true;
    }

    // ---- LightIntBand.dbc: 18 colour rows per param -------------------------------------------
    for (ParamWrite const& write_entry : writes)
    {
      SkyParam* const param = write_entry.param;

      if (!param->_colors_need_save && !param->_is_new_param_record)
        continue;

      int const first_row = lightIntBandFirstRow(param->Id);

      for (int i = 0; i < NUM_SkyColorNames; ++i)
      {
        DBCFile::Record record = recordForWriting(gLightIntBandDB, first_row + i);

        int const entries = static_cast<int>(param->colorRows[i].size());

        record.write(LightIntBandDB::Entries, entries);

        // All sixteen slots every time, so a band that shrank does not leave the client reading
        // the tail of the previous version.
        for (int l = 0; l < MAX_BAND_ENTRIES; ++l)
        {
          if (l >= entries)
          {
            record.write(LightIntBandDB::Times + l, 0);
            record.write(LightIntBandDB::Values + l, 0);
            continue;
          }

          record.write(LightIntBandDB::Times + l, param->colorRows[i][l].time);

          int const packed = static_cast<int>(param->colorRows[i][l].color.z * 255.0f)
                           + (static_cast<int>(param->colorRows[i][l].color.y * 255.0f) << 8)
                           + (static_cast<int>(param->colorRows[i][l].color.x * 255.0f) << 16);

          record.write(LightIntBandDB::Values + l, packed);
        }
      }

      wrote_colors_dbc = true;
    }

    // ---- LightFloatBand.dbc: 6 float rows per param -------------------------------------------
    for (ParamWrite const& write_entry : writes)
    {
      SkyParam* const param = write_entry.param;

      if (!param->_floats_need_save && !param->_is_new_param_record)
        continue;

      int const first_row = lightFloatBandFirstRow(param->Id);

      for (int i = 0; i < NUM_SkyFloatParamsNames; ++i)
      {
        DBCFile::Record record = recordForWriting(gLightFloatBandDB, first_row + i);

        int const entries = static_cast<int>(param->floatParams[i].size());

        record.write(LightFloatBandDB::Entries, entries);

        for (int l = 0; l < MAX_BAND_ENTRIES; ++l)
        {
          if (l >= entries)
          {
            record.write(LightFloatBandDB::Times + l, 0);
            record.write(LightFloatBandDB::Values + l, 0.0f);
            continue;
          }

          record.write(LightFloatBandDB::Times + l, param->floatParams[i][l].time);
          record.write(LightFloatBandDB::Values + l, param->floatParams[i][l].value);
        }
      }

      wrote_floats_dbc = true;
    }

    // ---- Light.dbc, last of the in-memory writes ----------------------------------------------
    //
    // The DataIDs loop is the defect that made this whole function dangerous. It ran to
    // NUM_SkyFloatParamsNames (6) over a uint[8] field, and its body called getCurrentParam()
    // rather than getParam(slot) -- so saving a light while the combo box showed "Storm Weather"
    // wrote the storm param's id into DataIDs[0..5], collapsing Clear, Clear-Underwater, Storm,
    // Storm-Underwater and Death onto one param, and left DataIDs[6..7] untouched. Every weather
    // state on that light became the same one, irreversibly, against the project DBC.
    //
    // All eight slots are written from skyParams[], zeroes included, so the row on disk always
    // matches the object in memory rather than retaining ids from a previous save.
    DBCFile::Record light_record = recordForWriting(gLightDB, Id);

    light_record.write(LightDB::Map, _map_id);
    light_record.write(LightDB::PositionX, pos.x * skymul);
    light_record.write(LightDB::PositionY, pos.y * skymul);
    light_record.write(LightDB::PositionZ, pos.z * skymul);
    light_record.write(LightDB::RadiusInner, r1 * skymul);
    light_record.write(LightDB::RadiusOuter, r2 * skymul);

    for (int slot = 0; slot < NUM_SkyParamsNames; ++slot)
    {
      light_record.write(LightDB::DataIDs + slot, static_cast<int>(skyParams[slot]));
    }

    // ---- disk: referenced files before the files that reference them --------------------------
    //
    // DBCFile::save() is void and non-throwing: it writes a sibling and renames, so a failure
    // leaves the previous file intact, but it cannot report that it failed. The remaining defence
    // is the order. Light.dbc goes LAST, because it is the row that makes everything else
    // reachable by the client -- if an earlier save silently failed, Light.dbc still points at the
    // old, self-consistent set and the user sees stale lighting. Saving Light.dbc first and losing
    // LightIntBand.dbc is the outcome that produces a light whose colour rows do not exist.
    if (wrote_skybox_dbc)
      gLightSkyboxDB.save();

    if (wrote_colors_dbc)
      gLightIntBandDB.save();

    if (wrote_floats_dbc)  // was gated on the colours flag; save_floats_dbc was computed and never read
      gLightFloatBandDB.save();

    if (wrote_param_dbc)
      gLightParamsDB.save();

    gLightDB.save();

    for (ParamWrite const& write_entry : writes)
    {
      // Only this one is cleared. _need_save / _colors_need_save / _floats_need_save stay true on
      // purpose: the colour and time-step editors live in LightViewWidget, which mutates
      // SkyParam::colorRows directly and sets no dirty flag anywhere, so clearing those here would
      // silently drop every colour edit made after the first save. Always-write is wasteful; it is
      // not lossy, and until LightViewWidget marks its own edits that is the correct trade.
      write_entry.param->_is_new_param_record = false;
    }

    is_new_record = false;

    std::size_t const param_rows = wrote_param_dbc ? writes.size() : std::size_t(0);
    std::size_t const int_rows
      = wrote_colors_dbc ? writes.size() * std::size_t(NUM_SkyColorNames) : std::size_t(0);
    std::size_t const float_rows
      = wrote_floats_dbc ? writes.size() * std::size_t(NUM_SkyFloatParamsNames) : std::size_t(0);

    Log << "Saved light " << Id << " (map " << _map_id << "): 1 Light.dbc row, " << param_rows
        << " LightParams row(s), " << int_rows << " LightIntBand row(s), " << float_rows
        << " LightFloatBand row(s)." << std::endl;

    return true;
  }
  catch (DBCFile::AlreadyExists)
  {
    LogError << "DBCFile::AlreadyExists while saving light " << Id
             << ". Validation had passed, so a DBC changed underneath this save." << std::endl;
  }
  catch (DBCFile::NotFound)
  {
    LogError << "DBCFile::NotFound while saving light " << Id
             << ". Validation had passed, so a DBC changed underneath this save." << std::endl;
  }
  catch (...)
  {
    LogError << "Unknown exception while saving light " << Id << "." << std::endl;
  }

  return false;
}
