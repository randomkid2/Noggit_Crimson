// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/UidCollisionLog.hpp>

#include <string>
#include <utility>

namespace
{
  // Duplicate uids come in per-tile bursts as a tile's MDDF/MODF entries load, so paying for one
  // allocation on the first collision covers a whole tile's worth without stepping through the
  // vector's early growth sizes.
  constexpr std::size_t INITIAL_CAPACITY = 64;
}

namespace Noggit
{
  void UidCollisionLog::record( std::uint32_t previous_uid
                              , std::uint32_t assigned_uid
                              , UidCollisionKind kind
                              , std::string_view model_name
                              , UidCollisionTile tile
                              )
  {
    std::lock_guard<std::mutex> const lock (_mutex);

    // Counted before the cap check so the total stays truthful even when the list stops growing.
    ++_total_count;

    if (_records.size() >= MAX_RECORDS)
    {
      // Nothing is copied past the cap. Taking the name as a view rather than a std::string is
      // what makes this branch free, which matters because the pathological case -- a fully
      // desynced uid.ini -- is exactly the one that reaches it.
      return;
    }

    if (_records.empty())
    {
      _records.reserve(INITIAL_CAPACITY);
    }

    UidCollisionRecord& stored = _records.emplace_back();
    stored.previous_uid = previous_uid;
    stored.assigned_uid = assigned_uid;
    stored.kind = kind;
    stored.model_name.assign(model_name);
    stored.tile = tile;
  }

  std::vector<UidCollisionRecord> UidCollisionLog::snapshot() const
  {
    std::lock_guard<std::mutex> const lock (_mutex);
    return _records;
  }

  std::size_t UidCollisionLog::recordedCount() const
  {
    std::lock_guard<std::mutex> const lock (_mutex);
    return _records.size();
  }

  std::size_t UidCollisionLog::totalCount() const
  {
    std::lock_guard<std::mutex> const lock (_mutex);
    return _total_count;
  }

  bool UidCollisionLog::truncated() const
  {
    std::lock_guard<std::mutex> const lock (_mutex);
    return _total_count > _records.size();
  }

  bool UidCollisionLog::empty() const
  {
    std::lock_guard<std::mutex> const lock (_mutex);
    return _total_count == 0;
  }

  void UidCollisionLog::clear()
  {
    std::lock_guard<std::mutex> const lock (_mutex);

    // The capacity is released as well: a log is cleared when the user has dealt with the report,
    // and holding MAX_RECORDS worth of strings afterwards serves nothing.
    _records = std::vector<UidCollisionRecord>();
    _total_count = 0;
  }

  std::string formatUidCollision(UidCollisionRecord const& record)
  {
    std::string line ("uid ");
    line += std::to_string(record.previous_uid);
    line += " -> ";
    line += std::to_string(record.assigned_uid);
    line += " (";
    line += (record.kind == UidCollisionKind::WMO ? "WMO" : "M2");
    line += ", tile ";

    if (record.tile.known)
    {
      line += std::to_string(record.tile.x);
      line += ',';
      line += std::to_string(record.tile.z);
    }
    else
    {
      line += "unknown";
    }

    line += ", ";
    line += record.model_name.empty() ? "<no model>" : record.model_name;
    line += ')';

    return line;
  }
}
