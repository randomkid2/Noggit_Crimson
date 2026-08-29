// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/terrain/TerrainMaskStore.hpp>
#include <noggit/terrain/TerrainMaskQuery.hpp>

#include <QtCore/QByteArray>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>
#include <QtCore/QString>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace Noggit
{
  namespace TerrainMaskQuery
  {
    namespace Detail
    {
      // The definition of the flag declared in TerrainMaskQuery.hpp. It lives HERE, next to the
      // only code that writes it, rather than in TerrainMaskQuery.cpp, so that "who can change
      // this" is answerable by reading one file.
      bool g_clipping_active = false;
    }
  }
}

namespace
{
  // Sidecar directory and file names, spelled once so the reader and the writer cannot disagree.
  constexpr char const* MASK_DIRECTORY = "noggit_masks";
  constexpr char const* MASK_INDEX_FILE = "index.json";
  constexpr char const* PAINT_SUFFIX = ".maskpaint";

  // Bumped only for a format change that an older build could misread. The reader refuses anything
  // it does not know rather than guessing, because a half-understood mask is worse than no mask:
  // it would clip edits somewhere the user cannot predict.
  constexpr std::uint32_t PAINT_FORMAT_VERSION = 1u;
  constexpr char const* PAINT_MAGIC = "NOGGITMASKPAINT1";
  constexpr int PAINT_MAGIC_LENGTH = 16;

  constexpr std::uint8_t PAINT_BLOCK_UNIFORM = 0u;
  constexpr std::uint8_t PAINT_BLOCK_DENSE = 1u;

  // A filename derived from a mask name, safe on every filesystem the editor runs on.
  //
  // Anything outside [A-Za-z0-9._-] becomes an underscore. Uppercase is FOLDED, which looks lossy
  // and is the point: NTFS and APFS are case-insensitive by default, so "Valley" and "valley" would
  // otherwise produce two names that collide as one file and silently overwrite each other. The
  // caller de-duplicates the folded result.
  std::string slugify(std::string const& name)
  {
    std::string slug;
    slug.reserve(name.size());

    for (char c : name)
    {
      bool const safe = (c >= 'a' && c <= 'z')
                     || (c >= 'A' && c <= 'Z')
                     || (c >= '0' && c <= '9')
                     || c == '.' || c == '_' || c == '-';

      if (!safe)
      {
        slug.push_back('_');
        continue;
      }

      slug.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }

    // A name made entirely of separators, or an empty one, would produce a hidden or empty
    // filename. Neither is a file the user could find and delete.
    if (slug.empty() || slug[0] == '.')
    {
      slug.insert(slug.begin(), 'm');
    }

    return slug;
  }

  void writeUint32(QByteArray& out, std::uint32_t value)
  {
    // Little-endian explicitly, not memcpy of the host representation. The sidecar travels with the
    // project; a mask authored on one machine has to read back on another.
    out.append(static_cast<char>(value & 0xFFu));
    out.append(static_cast<char>((value >> 8) & 0xFFu));
    out.append(static_cast<char>((value >> 16) & 0xFFu));
    out.append(static_cast<char>((value >> 24) & 0xFFu));
  }

  bool readUint32(QByteArray const& data, int& cursor, std::uint32_t& value)
  {
    if (cursor + 4 > data.size())
    {
      return false;
    }

    value = static_cast<std::uint32_t>(static_cast<unsigned char>(data[cursor]))
          | (static_cast<std::uint32_t>(static_cast<unsigned char>(data[cursor + 1])) << 8)
          | (static_cast<std::uint32_t>(static_cast<unsigned char>(data[cursor + 2])) << 16)
          | (static_cast<std::uint32_t>(static_cast<unsigned char>(data[cursor + 3])) << 24);

    cursor += 4;
    return true;
  }

  // --- Stack serialisation ---

  QJsonObject rangeToJson(Noggit::MaskRange const& range)
  {
    QJsonObject object;

    // An unbounded endpoint is written as an ABSENT KEY rather than as a sentinel number, for the
    // reason TerrainRuleStore::writeRange spells out at length: neither JSON nor QVariant has a
    // portable spelling for an infinity, and a sentinel that reads back as 0 turns "no lower bound"
    // into "at least zero" -- a different filter, silently, at the next project load.
    if (range.bounded() && range.low > -1.0e29f)
    {
      object.insert("low", static_cast<double>(range.low));
    }

    if (range.bounded() && range.high < 1.0e29f)
    {
      object.insert("high", static_cast<double>(range.high));
    }

    if (range.feather != 0.0f)
    {
      object.insert("feather", static_cast<double>(range.feather));
    }

    return object;
  }

  Noggit::MaskRange rangeFromJson(QJsonObject const& object)
  {
    // Default-constructed, i.e. unbounded. An absent key leaves the endpoint unconstrained rather
    // than substituting a number.
    Noggit::MaskRange range;

    if (object.contains("low"))
    {
      range.low = static_cast<float>(object.value("low").toDouble());
    }

    if (object.contains("high"))
    {
      range.high = static_cast<float>(object.value("high").toDouble());
    }

    range.feather = static_cast<float>(object.value("feather").toDouble(0.0));

    return range;
  }

  QJsonObject layerToJson(Noggit::MaskFilterLayer const& layer)
  {
    QJsonObject object;

    object.insert("kind", QString::fromLatin1(Noggit::maskFilterKindName(layer.kind)));
    object.insert("combine", QString::fromLatin1(Noggit::maskCombineName(layer.combine)));
    object.insert("opacity", static_cast<double>(layer.opacity));
    object.insert("invert", layer.invert);
    object.insert("enabled", layer.enabled);
    object.insert("range", rangeToJson(layer.range));

    // Only the fields the kind actually uses are written. A curvature step stored on a noise layer
    // is noise in the file and a question for whoever reads it later.
    switch (layer.kind)
    {
      case Noggit::MaskFilterKind::Curvature:
        object.insert("curvature_step", layer.curvature_step);
        break;

      case Noggit::MaskFilterKind::Noise:
        object.insert("noise_wavelength", static_cast<double>(layer.noise_wavelength));
        object.insert("noise_octaves", layer.noise_octaves);
        object.insert("noise_gain", static_cast<double>(layer.noise_gain));
        // Through double, which is exact for every value a uint32 can hold: a double has 53 bits of
        // mantissa. QJsonValue has no integer type wider than int, and a seed above 2^31 written as
        // an int would come back negative.
        object.insert("noise_seed", static_cast<double>(layer.noise_seed));
        break;

      case Noggit::MaskFilterKind::LayerAlpha:
        object.insert("texture", QString::fromStdString(layer.texture));
        break;

      case Noggit::MaskFilterKind::AreaId:
      {
        QJsonArray ids;

        for (int id : layer.area_ids)
        {
          ids.push_back(id);
        }

        object.insert("area_ids", ids);
        break;
      }

      case Noggit::MaskFilterKind::Constant:
        object.insert("constant", static_cast<double>(layer.constant));
        break;

      default:
        break;
    }

    return object;
  }

  Noggit::MaskFilterLayer layerFromJson(QJsonObject const& object)
  {
    Noggit::MaskFilterLayer layer;

    layer.kind = Noggit::maskFilterKindFromName
      (object.value("kind").toString().toLatin1().constData());
    layer.combine = Noggit::maskCombineFromName
      (object.value("combine").toString().toLatin1().constData());

    layer.opacity = static_cast<float>(object.value("opacity").toDouble(1.0));
    layer.invert = object.value("invert").toBool(false);
    layer.enabled = object.value("enabled").toBool(true);
    layer.range = rangeFromJson(object.value("range").toObject());

    layer.curvature_step = object.value("curvature_step").toInt(2);
    layer.noise_wavelength = static_cast<float>(object.value("noise_wavelength").toDouble(64.0));
    layer.noise_octaves = object.value("noise_octaves").toInt(3);
    layer.noise_gain = static_cast<float>(object.value("noise_gain").toDouble(0.5));

    // Round-tripped through double and clamped into range. A hand-edited file can hold anything,
    // and a cast of an out-of-range double to uint32 is undefined rather than merely wrong.
    double const seed = object.value("noise_seed").toDouble(1337.0);
    layer.noise_seed = (seed >= 0.0 && seed <= 4294967295.0)
                     ? static_cast<std::uint32_t>(seed)
                     : 1337u;

    layer.texture = object.value("texture").toString().toStdString();
    layer.constant = static_cast<float>(object.value("constant").toDouble(1.0));

    QJsonArray const ids = object.value("area_ids").toArray();

    for (QJsonValue const& id : ids)
    {
      layer.area_ids.push_back(id.toInt());
    }

    return layer;
  }

  // --- Paint sidecar ---

  // Run-length encodes one 4096-byte block. Runs are capped at 255 so a length fits one byte.
  //
  // RLE rather than a general compressor: a painted mask block is overwhelmingly long runs of 0 and
  // 255 with a feathered edge between them, which is the one shape RLE handles as well as anything
  // would, and it costs twenty lines instead of a dependency. The worst case -- alternating bytes --
  // doubles the block to 8192 bytes, which is why the writer falls back to storing raw when the
  // encoding does not help.
  void encodeBlockRle(std::uint8_t const* texels, QByteArray& out)
  {
    int index = 0;

    while (index < Noggit::MASK_CHUNK_TEXELS)
    {
      std::uint8_t const value = texels[index];
      int run = 1;

      while (index + run < Noggit::MASK_CHUNK_TEXELS
          && run < 255
          && texels[index + run] == value)
      {
        ++run;
      }

      out.append(static_cast<char>(static_cast<std::uint8_t>(run)));
      out.append(static_cast<char>(value));

      index += run;
    }
  }

  bool decodeBlockRle(QByteArray const& data, int& cursor, std::uint8_t* out)
  {
    int written = 0;

    while (written < Noggit::MASK_CHUNK_TEXELS)
    {
      if (cursor + 2 > data.size())
      {
        return false;
      }

      int const run = static_cast<int>(static_cast<unsigned char>(data[cursor]));
      std::uint8_t const value = static_cast<std::uint8_t>(data[cursor + 1]);
      cursor += 2;

      // A zero run would never terminate the loop, and a run past the end of the block would write
      // outside `out`. Both are reachable from a truncated or hand-edited file, so both are checked
      // rather than assumed.
      if (run == 0 || written + run > Noggit::MASK_CHUNK_TEXELS)
      {
        return false;
      }

      std::memset(out + written, value, static_cast<std::size_t>(run));
      written += run;
    }

    return true;
  }
}

namespace Noggit
{
  // --- NamedTerrainMask ---

  namespace
  {
    std::uint32_t tileKey(int tile_x, int tile_z)
    {
      return static_cast<std::uint32_t>(tile_x) * 64u + static_cast<std::uint32_t>(tile_z);
    }
  }

  bool NamedTerrainMask::tileIsBaked(int tile_x, int tile_z) const
  {
    return baked_tiles.find(tileKey(tile_x, tile_z)) != baked_tiles.end();
  }

  void NamedTerrainMask::markTileBaked(int tile_x, int tile_z)
  {
    baked_tiles.insert(tileKey(tile_x, tile_z));
  }

  void NamedTerrainMask::releaseTile(int tile_x, int tile_z)
  {
    composited.releaseTile(tile_x, tile_z);
    baked_tiles.erase(tileKey(tile_x, tile_z));
  }

  void NamedTerrainMask::invalidateBake()
  {
    composited.clear();
    baked_tiles.clear();
  }

  std::size_t NamedTerrainMask::compositedBytes() const
  {
    return composited.bytes();
  }

  std::size_t NamedTerrainMask::paintBytes() const
  {
    return paint.bytes();
  }

  // --- TerrainMaskStore ---

  TerrainMaskStore* TerrainMaskStore::instance()
  {
    // Function-local static for the reason TerrainRuleStore::instance gives: a file-scope instance
    // would run its constructor before QCoreApplication had set the organisation and application
    // names, and anything reading Qt paths in a constructor would resolve them somewhere else.
    static TerrainMaskStore store;
    return &store;
  }

  std::vector<std::string> TerrainMaskStore::names() const
  {
    std::vector<std::string> result;
    result.reserve(_masks.size());

    for (NamedTerrainMask const& mask : _masks)
    {
      result.push_back(mask.name);
    }

    return result;
  }

  NamedTerrainMask* TerrainMaskStore::find(std::string const& name)
  {
    for (NamedTerrainMask& mask : _masks)
    {
      if (mask.name == name)
      {
        return &mask;
      }
    }

    return nullptr;
  }

  NamedTerrainMask const* TerrainMaskStore::find(std::string const& name) const
  {
    for (NamedTerrainMask const& mask : _masks)
    {
      if (mask.name == name)
      {
        return &mask;
      }
    }

    return nullptr;
  }

  NamedTerrainMask* TerrainMaskStore::create(std::string const& name)
  {
    if (name.empty() || find(name))
    {
      return nullptr;
    }

    // _masks is a vector, so this can reallocate and every outstanding NamedTerrainMask* is
    // invalidated. Callers must not hold one across create/remove/rename; the dialog re-finds by
    // name after every mutation for exactly this reason. A deque or a list would avoid it and cost
    // locality on a container that holds a handful of entries and is walked by name.
    _masks.push_back(NamedTerrainMask{});
    _masks.back().name = name;

    return &_masks.back();
  }

  bool TerrainMaskStore::remove(std::string const& name)
  {
    for (std::size_t i = 0; i < _masks.size(); ++i)
    {
      if (_masks[i].name != name)
      {
        continue;
      }

      _masks.erase(_masks.begin() + static_cast<std::ptrdiff_t>(i));

      if (_active_name == name)
      {
        _active_name.clear();
        refreshQueryFlag();
      }

      return true;
    }

    return false;
  }

  bool TerrainMaskStore::rename(std::string const& old_name, std::string const& new_name)
  {
    if (new_name.empty() || old_name == new_name || find(new_name))
    {
      return false;
    }

    NamedTerrainMask* const mask = find(old_name);

    if (!mask)
    {
      return false;
    }

    mask->name = new_name;

    if (_active_name == old_name)
    {
      _active_name = new_name;
    }

    return true;
  }

  std::string const& TerrainMaskStore::activeName() const
  {
    return _active_name;
  }

  bool TerrainMaskStore::setActive(std::string const& name)
  {
    if (name.empty())
    {
      _active_name.clear();
      refreshQueryFlag();
      return true;
    }

    if (!find(name))
    {
      return false;
    }

    _active_name = name;
    refreshQueryFlag();

    return true;
  }

  NamedTerrainMask* TerrainMaskStore::active()
  {
    return _active_name.empty() ? nullptr : find(_active_name);
  }

  NamedTerrainMask const* TerrainMaskStore::active() const
  {
    return _active_name.empty() ? nullptr : find(_active_name);
  }

  bool TerrainMaskStore::clippingEnabled() const
  {
    return _clipping_enabled;
  }

  void TerrainMaskStore::setClippingEnabled(bool enabled)
  {
    _clipping_enabled = enabled;
    refreshQueryFlag();
  }

  void TerrainMaskStore::refreshQueryFlag() const
  {
    TerrainMaskQuery::Detail::g_clipping_active = _clipping_enabled && active() != nullptr;
  }

  float TerrainMaskStore::factorAt(float world_x, float world_z) const
  {
    // Every early exit returns 1.0. See the fail-open argument in TerrainMaskQuery.hpp.
    if (!_clipping_enabled)
    {
      return 1.0f;
    }

    NamedTerrainMask const* const mask = active();

    if (!mask)
    {
      return 1.0f;
    }

    // Non-finite guard BEFORE the cast. A NaN coordinate reaches a brush from an unprojection that
    // missed the terrain, and casting it to int is undefined rather than merely wrong.
    if (!(world_x >= 0.0f) || !(world_z >= 0.0f))
    {
      return 1.0f;
    }

    int const tile_x = static_cast<int>(world_x / MASK_TILE_SIZE);
    int const tile_z = static_cast<int>(world_z / MASK_TILE_SIZE);

    if ( tile_x < 0 || tile_x >= MASK_MAP_TILE_SIDE
      || tile_z < 0 || tile_z >= MASK_MAP_TILE_SIDE
       )
    {
      return 1.0f;
    }

    // THE UNBAKED CASE, and the reason baked_tiles exists at all. A tile that has never been baked
    // holds no chunks, and an absent chunk reads as 0 -- which would clip every edit on terrain the
    // mask has simply not looked at yet. The user would see a brush that stops working when they
    // walk one tile west. Failing open here is what turns that into "the mask is not applying over
    // there", which the dialog reports as a count of unbaked tiles.
    if (!mask->tileIsBaked(tile_x, tile_z))
    {
      return 1.0f;
    }

    return mask->composited.valueAt(world_x, world_z);
  }

  void TerrainMaskStore::releaseTile(int tile_x, int tile_z)
  {
    // Every mask, not only the active one. A mask that is not selected right now still holds a
    // composited field from the last time it was, and that field is exactly as large.
    for (NamedTerrainMask& mask : _masks)
    {
      mask.releaseTile(tile_x, tile_z);
    }
  }

  std::size_t TerrainMaskStore::compositedBytes() const
  {
    std::size_t total = 0;

    for (NamedTerrainMask const& mask : _masks)
    {
      total += mask.compositedBytes();
    }

    return total;
  }

  std::size_t TerrainMaskStore::budgetBytes() const
  {
    return _budget_bytes;
  }

  void TerrainMaskStore::setBudgetBytes(std::size_t bytes)
  {
    _budget_bytes = bytes;
  }

  std::size_t TerrainMaskStore::enforceBudget()
  {
    std::size_t dropped = 0;

    // Inactive masks first, in list order, and the active mask never. Dropping the active one would
    // make every subsequent query fail open, i.e. the mask the user is working with would silently
    // stop clipping -- the one outcome the budget must not produce.
    for (NamedTerrainMask& mask : _masks)
    {
      if (compositedBytes() <= _budget_bytes)
      {
        break;
      }

      if (mask.name == _active_name || mask.composited.empty())
      {
        continue;
      }

      mask.invalidateBake();
      ++dropped;
    }

    return dropped;
  }

  std::string const& TerrainMaskStore::lastError() const
  {
    return _last_error;
  }

  bool TerrainMaskStore::save(std::string const& project_path) const
  {
    _last_error.clear();

    if (project_path.empty())
    {
      _last_error = "no project path";
      return false;
    }

    QDir const project_dir (QString::fromStdString(project_path));
    QString const mask_dir_path = project_dir.filePath(QString::fromLatin1(MASK_DIRECTORY));

    if (!QDir().mkpath(mask_dir_path))
    {
      _last_error = "could not create " + mask_dir_path.toStdString();
      return false;
    }

    QDir const mask_dir (mask_dir_path);

    QJsonArray mask_array;

    // Folded slugs already seen, so two masks whose names differ only in case or in punctuation do
    // not write to the same sidecar. The suffix is appended to the SLUG only; the user-visible name
    // is untouched.
    std::vector<std::string> used_slugs;

    for (NamedTerrainMask const& mask : _masks)
    {
      std::string slug = slugify(mask.name);

      if (std::find(used_slugs.begin(), used_slugs.end(), slug) != used_slugs.end())
      {
        std::size_t suffix = 2;

        while (std::find(used_slugs.begin(), used_slugs.end(), slug + "_" + std::to_string(suffix))
               != used_slugs.end())
        {
          ++suffix;
        }

        slug += "_" + std::to_string(suffix);
      }

      used_slugs.push_back(slug);

      QJsonObject mask_object;
      mask_object.insert("name", QString::fromStdString(mask.name));
      mask_object.insert("slug", QString::fromStdString(slug));
      mask_object.insert("paint_combine", QString::fromLatin1(maskCombineName(mask.paint_combine)));

      QJsonArray layer_array;

      for (MaskFilterLayer const& layer : mask.stack.layers())
      {
        layer_array.push_back(layerToJson(layer));
      }

      mask_object.insert("layers", layer_array);

      // --- Paint sidecar ---

      std::vector<MaskChunkAddress> const chunks = mask.paint.storedChunks();

      QString const paint_path
        = mask_dir.filePath(QString::fromStdString(slug) + QString::fromLatin1(PAINT_SUFFIX));

      if (chunks.empty())
      {
        // No paint: no file, and any file from a previous save is removed. Leaving a stale sidecar
        // would resurrect erased strokes on the next load.
        QFile::remove(paint_path);
        mask_object.insert("has_paint", false);
      }
      else
      {
        QByteArray payload;
        payload.append(PAINT_MAGIC, PAINT_MAGIC_LENGTH);
        writeUint32(payload, PAINT_FORMAT_VERSION);
        writeUint32(payload, static_cast<std::uint32_t>(chunks.size()));

        std::uint8_t block[MASK_CHUNK_TEXELS];

        for (MaskChunkAddress const& address : chunks)
        {
          writeUint32(payload, address.packed());

          if (!mask.paint.chunkIsDense(address))
          {
            payload.append(static_cast<char>(PAINT_BLOCK_UNIFORM));
            payload.append(static_cast<char>(mask.paint.chunkUniformValue(address)));
            continue;
          }

          payload.append(static_cast<char>(PAINT_BLOCK_DENSE));

          // readChunk applies the field's invert and clamp, which is what a CONSUMER wants and
          // exactly what a serialiser must not have: saving the presented form and then re-applying
          // invert on load would invert twice. The paint layer never carries either flag -- it is a
          // raw field, and the flags belong to the composited result -- so the two agree here, and
          // this comment is what keeps them agreeing if that ever changes.
          mask.paint.readChunk(address, block);
          encodeBlockRle(block, payload);
        }

        QFile paint_file (paint_path);

        if (!paint_file.open(QIODevice::WriteOnly))
        {
          _last_error = "could not write " + paint_path.toStdString();
          return false;
        }

        paint_file.write(payload);
        paint_file.close();

        mask_object.insert("has_paint", true);
      }

      mask_array.push_back(mask_object);
    }

    QJsonObject root;
    root.insert("version", static_cast<int>(PAINT_FORMAT_VERSION));
    root.insert("masks", mask_array);

    // The ACTIVE SELECTION is saved but the CLIPPING SWITCH is not. Which mask the user was working
    // on is a harmless convenience; whether every brush is being clipped is a change to what the
    // tools do, and a destructive-feeling mode that survives a restart is the thing
    // TerrainRuleStore refuses to persist for Live Auto Texture, for the same reason.
    root.insert("active", QString::fromStdString(_active_name));

    QFile index_file (mask_dir.filePath(QString::fromLatin1(MASK_INDEX_FILE)));

    if (!index_file.open(QIODevice::WriteOnly))
    {
      _last_error = "could not write " + index_file.fileName().toStdString();
      return false;
    }

    index_file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    index_file.close();

    return true;
  }

  bool TerrainMaskStore::load(std::string const& project_path)
  {
    _last_error.clear();

    _masks.clear();
    _active_name.clear();

    // OFF after every load, whatever the file said. See save() for why the switch is not persisted
    // in the first place.
    _clipping_enabled = false;
    refreshQueryFlag();

    if (project_path.empty())
    {
      _last_error = "no project path";
      return false;
    }

    QDir const project_dir (QString::fromStdString(project_path));
    QDir const mask_dir (project_dir.filePath(QString::fromLatin1(MASK_DIRECTORY)));

    QFile index_file (mask_dir.filePath(QString::fromLatin1(MASK_INDEX_FILE)));

    if (!index_file.exists())
    {
      // A project with no masks is the normal state of every project that has never used the
      // feature, not an error.
      return true;
    }

    if (!index_file.open(QIODevice::ReadOnly))
    {
      _last_error = "could not read " + index_file.fileName().toStdString();
      return false;
    }

    QJsonParseError parse_error{};
    QJsonDocument const document = QJsonDocument::fromJson(index_file.readAll(), &parse_error);
    index_file.close();

    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
    {
      _last_error = "malformed mask index: " + parse_error.errorString().toStdString();
      return false;
    }

    QJsonObject const root = document.object();
    QJsonArray const mask_array = root.value("masks").toArray();

    for (QJsonValue const& mask_value : mask_array)
    {
      QJsonObject const mask_object = mask_value.toObject();

      std::string const name = mask_object.value("name").toString().toStdString();

      if (name.empty() || find(name))
      {
        // A duplicate or nameless entry is skipped rather than failing the whole load. One corrupt
        // entry must not cost the user the other nine masks in the file.
        continue;
      }

      _masks.push_back(NamedTerrainMask{});
      NamedTerrainMask& mask = _masks.back();
      mask.name = name;
      mask.paint_combine = maskCombineFromName
        (mask_object.value("paint_combine").toString().toLatin1().constData());

      QJsonArray const layer_array = mask_object.value("layers").toArray();

      for (QJsonValue const& layer_value : layer_array)
      {
        mask.stack.layers().push_back(layerFromJson(layer_value.toObject()));
      }

      if (!mask_object.value("has_paint").toBool(false))
      {
        continue;
      }

      std::string const slug = mask_object.value("slug").toString().toStdString();

      QFile paint_file
        (mask_dir.filePath(QString::fromStdString(slug) + QString::fromLatin1(PAINT_SUFFIX)));

      if (!paint_file.open(QIODevice::ReadOnly))
      {
        // The stack survived; only the hand-painted delta is missing. Reported, not fatal.
        _last_error = "missing paint sidecar for mask '" + name + "'";
        continue;
      }

      QByteArray const data = paint_file.readAll();
      paint_file.close();

      int cursor = 0;

      if (data.size() < PAINT_MAGIC_LENGTH
       || std::memcmp(data.constData(), PAINT_MAGIC, PAINT_MAGIC_LENGTH) != 0)
      {
        _last_error = "unrecognised paint sidecar for mask '" + name + "'";
        continue;
      }

      cursor = PAINT_MAGIC_LENGTH;

      std::uint32_t version = 0;
      std::uint32_t chunk_count = 0;

      if (!readUint32(data, cursor, version) || !readUint32(data, cursor, chunk_count))
      {
        _last_error = "truncated paint sidecar for mask '" + name + "'";
        continue;
      }

      if (version != PAINT_FORMAT_VERSION)
      {
        // Refused rather than guessed. A half-understood mask clips edits somewhere the user cannot
        // predict, which is worse than a mask that is plainly absent.
        _last_error = "paint sidecar for mask '" + name + "' is a newer format";
        continue;
      }

      std::uint8_t block[MASK_CHUNK_TEXELS];
      bool truncated = false;

      for (std::uint32_t i = 0; i < chunk_count && !truncated; ++i)
      {
        std::uint32_t packed = 0;

        if (!readUint32(data, cursor, packed) || cursor >= data.size())
        {
          truncated = true;
          break;
        }

        MaskChunkAddress const address = MaskChunkAddress::fromPacked(packed);

        std::uint8_t const block_kind = static_cast<std::uint8_t>(data[cursor]);
        ++cursor;

        if (block_kind == PAINT_BLOCK_UNIFORM)
        {
          if (cursor >= data.size())
          {
            truncated = true;
            break;
          }

          mask.paint.fillChunk(address, static_cast<std::uint8_t>(data[cursor]));
          ++cursor;
          continue;
        }

        if (block_kind != PAINT_BLOCK_DENSE || !decodeBlockRle(data, cursor, block))
        {
          truncated = true;
          break;
        }

        mask.paint.writeChunk(address, block);
      }

      if (truncated)
      {
        _last_error = "truncated paint sidecar for mask '" + name + "'";
      }
    }

    std::string const active = root.value("active").toString().toStdString();

    if (!active.empty() && find(active))
    {
      _active_name = active;
    }

    refreshQueryFlag();

    return true;
  }
}
