// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/database/DisplayResolver.hpp>

#include <noggit/DBC.h>
#include <noggit/DBCFile.h>
#include <noggit/database/ModelPathFixup.hpp>

#include <cmath>
#include <exception>
#include <string>

using namespace Noggit::Database;

namespace
{
  // Highest field index each chain reads, so the guard below is checkable by eye against the
  // contract. Expected 3.3.5.12340 field counts, from dist/definitions/*.dbd, are given for
  // reference; they are NOT asserted, because a lenient read that degrades to "no usable model
  // name" is friendlier than refusing to draw anything on a client whose DBCs are slightly off.
  constexpr std::size_t CREATURE_DISPLAY_INFO_FIELDS_NEEDED = 5;   // 3.3.5 has 16
  constexpr std::size_t CREATURE_MODEL_DATA_FIELDS_NEEDED = 5;     // 3.3.5 has 28
  constexpr std::size_t GAMEOBJECT_DISPLAY_INFO_FIELDS_NEEDED = 2; // 3.3.5 has 19

  // Ceiling on a string-table offset we are willing to dereference.
  //
  // DBCFile::Record::getString (DBCFile.cpp:318-324) reads the field as a byte offset, checks it
  // against the table size with assert() only, and returns stringTable.data() + offset. In
  // RelWithDebInfo the assert is gone, so a field that is not really a string offset produces a
  // pointer of essentially arbitrary distance from the table and a std::string built from it
  // reads unmapped memory.
  //
  // There is no public accessor for the string table's size -- see the report note on
  // DBCFile -- so the offset cannot be checked properly from outside. This cap is the next best
  // thing and it catches the failure that actually happens: a float field misread as a string
  // offset. A CreatureModelScale of 1.0 has the bit pattern 0x3F800000, which is 1065353216 --
  // sixty-three times this cap. No real 3.3.5 DBC string table comes within an order of
  // magnitude of 16 MB, so nothing legitimate is refused.
  constexpr std::uint32_t MAX_PLAUSIBLE_STRING_OFFSET = 16u * 1024u * 1024u;

  ResolvedModel failure(std::string reason)
  {
    ResolvedModel result;

    result.resolved = false;
    result.scale = 1.0;

    // The struct's contract is that failure_reason is non-empty exactly when !resolved, and a
    // caller is allowed to test the string instead of the flag. A reason that came out empty
    // would quietly break that, so it is backfilled rather than trusted.
    result.failure_reason = reason.empty() ? std::string("unspecified resolution failure") : std::move(reason);

    return result;
  }

  // A scale that will not make the model vanish or explode.
  //
  // Reading a float field at the wrong index yields a denormal, a NaN or something astronomical
  // rather than an obviously bad number, and some real CreatureDisplayInfo rows carry 0. Any of
  // those becomes 1.0: a model at the wrong size is a bug someone can see and report, a model
  // scaled to nothing is a bug they report as "spawns do not render".
  double usableScale(double scale)
  {
    return (std::isfinite(scale) && scale > 0.0) ? scale : 1.0;
  }

  // Reads a DBC string field into a bounded std::string, or returns empty when the field cannot
  // be trusted. See MAX_PLAUSIBLE_STRING_OFFSET for why every step here is necessary.
  std::string boundedString(DBCFile::Record const& record, std::size_t field)
  {
    std::uint32_t const offset = record.getUInt(field);

    // Offset 0 is the string table's leading NUL by convention, meaning "this row has no string".
    // Handling it here means the overwhelmingly common empty case never dereferences at all.
    if (offset == 0u || offset > MAX_PLAUSIBLE_STRING_OFFSET)
    {
      return {};
    }

    char const* raw = record.getString(field);

    if (raw == nullptr)
    {
      return {};
    }

    // Bounded scan rather than strlen: an offset landing near the end of the table gives a
    // sequence with no terminator before the buffer ends, and strlen would run off it.
    std::size_t length = 0;

    while (length < ModelPathFixup::MAX_MODEL_PATH_LENGTH && raw[length] != '\0')
    {
      ++length;
    }

    // Unterminated within the cap. Truncating would hand a plausible-looking prefix to the model
    // loader; refusing names the row instead.
    if (length == ModelPathFixup::MAX_MODEL_PATH_LENGTH)
    {
      return {};
    }

    return std::string(raw, length);
  }

  // Common front half of every lookup: is the file usable, and does it have the row.
  //
  // Reported separately from "row not found" because the two need different fixes. An empty file
  // means OpenDBs has not run or the DBC is missing from the archive chain -- which today looks
  // identical to a missing row, since getByID on an empty file walks zero records and throws
  // NotFound just the same.
  bool databaseIsLoaded(DBCFile& database)
  {
    return database.getRecordCount() > 0;
  }

  // The three replaceable texture slots a creature display id can fill, in TextureVariation order.
  //
  // 11, 12, 13 are the M2 texture types for monster skins 1 to 3. They are matched against
  // Model::_replaceable_texture_types at draw time, not by position, because a model may declare
  // them in any order and may declare only some of them.
  constexpr int MONSTER_SKIN_TYPES[] = {11, 12, 13};
  constexpr std::size_t MONSTER_SKIN_COUNT = 3;

  // Turns TextureVariation entries into openable .blp keys.
  //
  // The variation is a bare name with no directory and no extension -- "RatSkinBrown" -- and the
  // image lives beside the model. So the path is the model's own directory plus the name plus
  // ".blp". That is a client convention rather than something the DBC records, which is exactly
  // why it is written down here.
  std::vector<std::pair<int, std::string>> skinsFor
    ( std::vector<std::string> const& variations
    , std::string const& model_path
    )
  {
    std::vector<std::pair<int, std::string>> skins;

    // model_path has already been through ModelPathFixup, so separators are backslashes and there
    // is exactly one spelling to look for.
    std::size_t const slash (model_path.rfind('\\'));

    if (slash == std::string::npos)
    {
      // A model at the archive root has no directory to put a skin beside. No real creature model
      // is stored that way, and guessing a directory would produce a miss attributed to the model
      // loader rather than to this row.
      return skins;
    }

    std::string const directory (model_path.substr(0, slash + 1));

    for (std::size_t n = 0; n < variations.size() && n < MONSTER_SKIN_COUNT; ++n)
    {
      if (variations[n].empty())
      {
        continue;
      }

      // Guard the join rather than trusting the DBC: a variation carrying a separator would climb
      // out of the model's directory, and one carrying an extension would produce "foo.blp.blp".
      if (variations[n].find('\\') != std::string::npos
       || variations[n].find('/') != std::string::npos
       || variations[n].find('.') != std::string::npos)
      {
        continue;
      }

      skins.emplace_back(MONSTER_SKIN_TYPES[n], directory + variations[n] + ".blp");
    }

    return skins;
  }

  ResolvedModel resolveCreatureChain
    ( DBCFile& display_info
    , DBCFile& model_data
    , std::uint32_t display_id
    )
  {
    if (!databaseIsLoaded(display_info))
    {
      return failure("CreatureDisplayInfo.dbc is empty or was never opened");
    }

    if (display_info.getFieldCount() < CREATURE_DISPLAY_INFO_FIELDS_NEEDED)
    {
      return failure("CreatureDisplayInfo.dbc has " + std::to_string(display_info.getFieldCount())
                     + " fields, too few for this client build");
    }

    std::uint32_t model_id = 0;
    double display_scale = 1.0;
    std::vector<std::string> texture_variations;

    // TextureVariation occupies fields 6-8, well past the five the scale and model id need, so a
    // client whose CreatureDisplayInfo is narrower than that still resolves a model -- it simply
    // renders without a skin, exactly as it did before skins were read at all.
    bool const has_variations
      (display_info.getFieldCount() > CreatureDisplayInfoDB::TextureVariation + 2);

    try
    {
      DBCFile::Record const display (display_info.getByID(display_id));

      model_id = display.getUInt(CreatureDisplayInfoDB::ModelID);
      display_scale = usableScale
        (static_cast<double>(display.getFloat(CreatureDisplayInfoDB::CreatureModelScale)));

      if (has_variations)
      {
        for (std::size_t n = 0; n < MONSTER_SKIN_COUNT; ++n)
        {
          texture_variations.push_back
            (boundedString(display, CreatureDisplayInfoDB::TextureVariation + n));
        }
      }
    }
    catch (DBCFile::NotFound const&)
    {
      return failure("no CreatureDisplayInfo row for display id " + std::to_string(display_id));
    }
    catch (std::exception const& e)
    {
      return failure("CreatureDisplayInfo lookup for display id " + std::to_string(display_id)
                     + " failed: " + e.what());
    }
    catch (...)
    {
      // getByID's own failure path is a std::runtime_error, but the Record accessors are raw
      // pointer arithmetic and the DBC could have been reopened underneath us. A catch-all is
      // what keeps the guarantee in the header true.
      return failure("CreatureDisplayInfo lookup for display id " + std::to_string(display_id)
                     + " failed with an unknown exception");
    }

    if (model_id == 0)
    {
      return failure("CreatureDisplayInfo " + std::to_string(display_id) + " has no ModelID");
    }

    if (!databaseIsLoaded(model_data))
    {
      return failure("CreatureModelData.dbc is empty or was never opened");
    }

    if (model_data.getFieldCount() < CREATURE_MODEL_DATA_FIELDS_NEEDED)
    {
      return failure("CreatureModelData.dbc has " + std::to_string(model_data.getFieldCount())
                     + " fields, too few for this client build");
    }

    std::string raw_name;
    double model_scale = 1.0;

    try
    {
      DBCFile::Record const model (model_data.getByID(model_id));

      // Field 2, not 1. Flags precedes ModelName in WotLK, so index 1 would hand an integer to
      // getString and be interpreted as a string-table offset. Verified against
      // dist/definitions/CreatureModelData.dbd, BUILD 3.1.0.9767-3.3.5.12340.
      raw_name = boundedString(model, CreatureModelDataDB::ModelName);
      model_scale = usableScale
        (static_cast<double>(model.getFloat(CreatureModelDataDB::ModelScale)));
    }
    catch (DBCFile::NotFound const&)
    {
      return failure("CreatureDisplayInfo " + std::to_string(display_id) + " points at "
                     "CreatureModelData " + std::to_string(model_id) + ", which does not exist");
    }
    catch (std::exception const& e)
    {
      return failure("CreatureModelData lookup for model id " + std::to_string(model_id)
                     + " failed: " + e.what());
    }
    catch (...)
    {
      return failure("CreatureModelData lookup for model id " + std::to_string(model_id)
                     + " failed with an unknown exception");
    }

    std::string model_path (ModelPathFixup::toM2Path(raw_name));

    if (model_path.empty())
    {
      // Reached when the name is blank, or when the field is not really a string -- which is what
      // happens if a Cataclysm-or-later CreatureModelData.dbc is in the archive chain, since
      // FileDataID took ModelName's place at index 2 there.
      return failure("CreatureModelData " + std::to_string(model_id)
                     + " has no usable ModelName");
    }

    ResolvedModel result;

    result.resolved = true;
    result.model_path = std::move(model_path);

    // Both factors multiply. CreatureDisplayInfo.CreatureModelScale is the per-display
    // adjustment; CreatureModelData.ModelScale is the model's own base size. The client applies
    // both, so using either alone gets a visibly wrong size on every creature where they differ.
    // Sanitised again after multiplying, because two finite factors can still overflow to inf.
    result.scale = usableScale(display_scale * model_scale);

    // Built from the final path, so the skins sit beside whatever model actually resolved.
    result.skins = skinsFor(texture_variations, result.model_path);

    return result;
  }

  ResolvedModel resolveGameObjectChain(DBCFile& display_info, std::uint32_t display_id)
  {
    if (!databaseIsLoaded(display_info))
    {
      return failure("GameObjectDisplayInfo.dbc is empty or was never opened");
    }

    if (display_info.getFieldCount() < GAMEOBJECT_DISPLAY_INFO_FIELDS_NEEDED)
    {
      return failure("GameObjectDisplayInfo.dbc has " + std::to_string(display_info.getFieldCount())
                     + " fields, too few for this client build");
    }

    std::string raw_name;

    try
    {
      DBCFile::Record const display (display_info.getByID(display_id));

      raw_name = boundedString(display, GameObjectDisplayInfoDB::ModelName);
    }
    catch (DBCFile::NotFound const&)
    {
      return failure("no GameObjectDisplayInfo row for display id " + std::to_string(display_id));
    }
    catch (std::exception const& e)
    {
      return failure("GameObjectDisplayInfo lookup for display id " + std::to_string(display_id)
                     + " failed: " + e.what());
    }
    catch (...)
    {
      return failure("GameObjectDisplayInfo lookup for display id " + std::to_string(display_id)
                     + " failed with an unknown exception");
    }

    std::string model_path (ModelPathFixup::toM2Path(raw_name));

    if (model_path.empty())
    {
      return failure("GameObjectDisplayInfo " + std::to_string(display_id)
                     + " has no usable ModelName");
    }

    ResolvedModel result;

    result.resolved = true;
    result.model_path = std::move(model_path);

    // No scale in this chain. GameObjectDisplayInfo carries a GeoBox, not a scale; the size
    // multiplier for a gameobject is gameobject_template.size, which is world-DB data and
    // belongs to the spawn, not to the display id. Returning 1.0 here keeps the two sources
    // separate rather than half-applying one of them.
    result.scale = 1.0;

    return result;
  }
}

ResolvedModel DisplayResolver::creatureModel(std::uint32_t display_id)
{
  if (display_id == 0)
  {
    // Not cached and not an error. creature.modelid is 0 on most rows, meaning "use the
    // template", and creature_template.modelidN is 0 for unused slots. Answering directly is
    // cheaper than a hash lookup and keeps cacheSize() meaning "ids we actually went to the DBC
    // for".
    return failure("display id 0: no model");
  }

  auto const cached = _creature_cache.find(display_id);

  if (cached != _creature_cache.end())
  {
    return cached->second;
  }

  ResolvedModel result
    (resolveCreatureChain(gCreatureDisplayInfoDB, gCreatureModelDataDB, display_id));

  _creature_cache.emplace(display_id, result);

  return result;
}

ResolvedModel DisplayResolver::gameObjectModel(std::uint32_t display_id)
{
  if (display_id == 0)
  {
    return failure("display id 0: no model");
  }

  auto const cached = _gameobject_cache.find(display_id);

  if (cached != _gameobject_cache.end())
  {
    return cached->second;
  }

  ResolvedModel result (resolveGameObjectChain(gGameObjectDisplayInfoDB, display_id));

  _gameobject_cache.emplace(display_id, result);

  return result;
}

void DisplayResolver::clearCache()
{
  _creature_cache.clear();
  _gameobject_cache.clear();
}

std::size_t DisplayResolver::cacheSize() const
{
  return _creature_cache.size() + _gameobject_cache.size();
}
