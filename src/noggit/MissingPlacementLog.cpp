// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/MissingPlacementLog.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace
{
  // A broken patch fails in bursts -- a whole doodad set at once -- so paying for one allocation
  // on the first failure covers the burst without stepping through the vector's early growth
  // sizes. Same reasoning as UidCollisionLog.cpp.
  constexpr std::size_t INITIAL_FILE_CAPACITY = 32;
  constexpr std::size_t INITIAL_PLACEMENT_CAPACITY = 256;

  // Positions are quantised to a tenth of a yard before they enter the dedup key. Two walks of
  // the same tile produce bit-identical floats, so this is not about tolerance; it is so that
  // the key is a short, stable string rather than the full float repr, and so a placement that
  // the mapper nudged by a hair does not become a second row.
  constexpr float DEDUP_QUANTISATION = 10.0f;

  std::string quantise(float v)
  {
    return std::to_string(static_cast<long long>(std::lround(v * DEDUP_QUANTISATION)));
  }

  std::string extensionOf(std::string const& normalised)
  {
    std::size_t const dot = normalised.find_last_of('.');

    if (dot == std::string::npos)
    {
      return {};
    }

    return normalised.substr(dot);
  }
}

namespace Noggit
{
  std::string normaliseAssetPath(std::string_view path)
  {
    std::string out;
    out.reserve(path.size());

    for (char const c : path)
    {
      if (c == '\\')
      {
        out.push_back('/');
      }
      else if (c >= 'A' && c <= 'Z')
      {
        // Not std::tolower: that is locale-dependent, and a Turkish locale maps 'I' to a dotless
        // i, which would make two spellings of the same path hash differently. Asset paths are
        // ASCII.
        out.push_back(static_cast<char>(c - 'A' + 'a'));
      }
      else
      {
        out.push_back(c);
      }
    }

    return out;
  }

  bool classifyMissingAsset(std::string_view path, MissingAssetKind& kind_out)
  {
    std::string const normalised (normaliseAssetPath(path));
    std::string const extension (extensionOf(normalised));

    if (extension == ".wmo")
    {
      kind_out = MissingAssetKind::WorldModel;
      return true;
    }

    // .mdx and .mdl are the pre-Wrath spellings. Noggit rewrites them to .m2 before loading in
    // most paths, but not all of them, and a placement that still carries the old extension is
    // exactly the kind of thing that fails to load.
    if (extension == ".m2" || extension == ".mdx" || extension == ".mdl")
    {
      kind_out = MissingAssetKind::Model;
      return true;
    }

    return false;
  }

  char const* missingPlacementKindLabel(MissingPlacementKind kind)
  {
    switch (kind)
    {
      case MissingPlacementKind::WorldModel:
        return "WMO";
      case MissingPlacementKind::WorldModelDoodad:
        return "WMO doodad";
      case MissingPlacementKind::Model:
      default:
        return "M2";
    }
  }

  char const* missingAssetStateLabel(MissingAssetState state)
  {
    switch (state)
    {
      case MissingAssetState::Missing:
        return "Missing";
      case MissingAssetState::Unreadable:
        return "Unreadable";
      case MissingAssetState::Unknown:
      default:
        return "Not probed";
    }
  }

  MissingPlacementLog& MissingPlacementLog::instance()
  {
    static MissingPlacementLog log;
    return log;
  }

  bool MissingPlacementLog::recordFileFailure(std::string_view file_path)
  {
    MissingAssetKind kind = MissingAssetKind::Model;

    if (!classifyMissingAsset(file_path, kind))
    {
      return false;
    }

    std::string key (normaliseAssetPath(file_path));

    std::lock_guard<std::mutex> const lock (_mutex);

    if (_files.find(key) != _files.end())
    {
      return false;
    }

    if (_files.size() >= MAX_FILES)
    {
      return false;
    }

    if (_files.empty())
    {
      _files.reserve(INITIAL_FILE_CAPACITY);
      _file_order.reserve(INITIAL_FILE_CAPACITY);
    }

    MissingFileRecord record;
    record.file_path.assign(file_path);
    record.kind = kind;

    _file_order.push_back(key);
    _files.emplace(std::move(key), std::move(record));

    _generation.fetch_add(1, std::memory_order_relaxed);

    return true;
  }

  bool MissingPlacementLog::recordPlacement(MissingPlacementRecord record)
  {
    std::string const key
      ( normaliseAssetPath(record.file_path) + '|'
      + std::to_string(record.owner_uid) + '|'
      + quantise(record.x) + ',' + quantise(record.y) + ',' + quantise(record.z) + '|'
      + missingPlacementKindLabel(record.kind)
      );

    std::lock_guard<std::mutex> const lock (_mutex);

    if (!_placement_keys.insert(key).second)
    {
      return false;
    }

    // Counted before the cap check, and the per-file count with it, so both stay truthful when
    // the listed rows stop growing.
    ++_total_placement_count;

    std::string const file_key (normaliseAssetPath(record.file_path));
    auto const file_it = _files.find(file_key);

    if (file_it != _files.end())
    {
      ++file_it->second.placement_count;
    }

    if (_placements.size() >= MAX_PLACEMENTS)
    {
      return false;
    }

    if (_placements.empty())
    {
      _placements.reserve(INITIAL_PLACEMENT_CAPACITY);
    }

    _placements.push_back(std::move(record));

    _generation.fetch_add(1, std::memory_order_relaxed);

    return true;
  }

  void MissingPlacementLog::setFileState(std::string_view file_path, MissingAssetState state)
  {
    std::string const key (normaliseAssetPath(file_path));

    std::lock_guard<std::mutex> const lock (_mutex);

    auto const it = _files.find(key);

    if (it == _files.end() || it->second.state == state)
    {
      return;
    }

    it->second.state = state;

    _generation.fetch_add(1, std::memory_order_relaxed);
  }

  std::uint64_t MissingPlacementLog::generation() const
  {
    return _generation.load(std::memory_order_relaxed);
  }

  std::vector<MissingFileRecord> MissingPlacementLog::files() const
  {
    std::lock_guard<std::mutex> const lock (_mutex);

    std::vector<MissingFileRecord> out;
    out.reserve(_file_order.size());

    for (std::string const& key : _file_order)
    {
      auto const it = _files.find(key);

      if (it != _files.end())
      {
        out.push_back(it->second);
      }
    }

    return out;
  }

  std::vector<MissingPlacementRecord> MissingPlacementLog::placements() const
  {
    std::lock_guard<std::mutex> const lock (_mutex);
    return _placements;
  }

  std::size_t MissingPlacementLog::recordedPlacementCount() const
  {
    std::lock_guard<std::mutex> const lock (_mutex);
    return _placements.size();
  }

  std::size_t MissingPlacementLog::totalPlacementCount() const
  {
    std::lock_guard<std::mutex> const lock (_mutex);
    return _total_placement_count;
  }

  std::size_t MissingPlacementLog::fileCount() const
  {
    std::lock_guard<std::mutex> const lock (_mutex);
    return _files.size();
  }

  bool MissingPlacementLog::truncated() const
  {
    std::lock_guard<std::mutex> const lock (_mutex);
    return _total_placement_count > _placements.size() || _files.size() >= MAX_FILES;
  }

  bool MissingPlacementLog::empty() const
  {
    std::lock_guard<std::mutex> const lock (_mutex);
    return _files.empty();
  }

  void MissingPlacementLog::clear()
  {
    std::lock_guard<std::mutex> const lock (_mutex);

    _files.clear();
    _file_order.clear();
    _placements.clear();
    _placement_keys.clear();
    _total_placement_count = 0;

    _generation.fetch_add(1, std::memory_order_relaxed);
  }

  bool MissingPlacementLog::drawPlaceholders() const
  {
    return _draw_placeholders.load(std::memory_order_relaxed);
  }

  void MissingPlacementLog::setDrawPlaceholders(bool draw)
  {
    _draw_placeholders.store(draw, std::memory_order_relaxed);
  }

  std::string formatMissingPlacement(MissingPlacementRecord const& record)
  {
    std::string out (missingPlacementKindLabel(record.kind));
    out += "  ";
    out += record.file_path;

    if (!record.owner_path.empty())
    {
      out += "  (inside ";
      out += record.owner_path;
      out += ")";
    }

    out += "  uid ";
    out += std::to_string(record.owner_uid);

    char coords[96] = {};
    std::snprintf(coords, sizeof(coords), "  at %.2f, %.2f, %.2f", record.x, record.y, record.z);
    out += coords;

    if (record.tile.known)
    {
      out += "  ADT ";
      out += std::to_string(record.tile.x);
      out += ",";
      out += std::to_string(record.tile.z);
    }
    else
    {
      // Not "ADT 0,0", and not silence: a placement whose position falls outside the 64x64 grid
      // is a real defect and naming it is the point of the row.
      out += "  outside the map grid";
    }

    return out;
  }
}
