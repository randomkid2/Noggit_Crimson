// This file is part of Noggit3, licensed under GNU General Public License (version 3).

// Tests for the uid collision recorder.
//
// The recorder is the only half of the uid-collision story that can be tested on a bare machine:
// the repair itself needs a World, a MapIndex and loaded instances, while the log is STL only.
// What is pinned here is everything a report depends on and nothing about the renumbering, which
// is deliberately untouched -- see world_model_instances_storage.cpp.
//
// Two properties matter more than the rest and are easy to regress:
//   - the total count stays truthful past the record cap, because that count is the number the
//     user is shown ("47000 uids were renumbered"), not the number the list can hold;
//   - record() takes a std::string_view and must copy it, because the caller passes a view into a
//     FileKey owned by a model that can be unloaded long before the report is opened.

#include <catch2/catch_test_macros.hpp>

#include <noggit/UidCollisionLog.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace Noggit;

namespace
{
  UidCollisionTile knownTile(std::uint32_t x, std::uint32_t z)
  {
    return UidCollisionTile{ x, z, true };
  }

  UidCollisionTile unknownTile()
  {
    return UidCollisionTile{};
  }
}

TEST_CASE("a fresh uid collision log holds nothing", "[uid]")
{
  UidCollisionLog log;

  CHECK(log.empty());
  CHECK(log.recordedCount() == 0);
  CHECK(log.totalCount() == 0);
  CHECK_FALSE(log.truncated());
  CHECK(log.snapshot().empty());
}

TEST_CASE("a recorded collision keeps every field it was given", "[uid]")
{
  UidCollisionLog log;
  log.record(1234, 5000, UidCollisionKind::M2, "world/wmo/foo.m2", knownTile(32, 48));

  REQUIRE(log.recordedCount() == 1);
  CHECK(log.totalCount() == 1);
  CHECK_FALSE(log.empty());

  auto const records = log.snapshot();
  REQUIRE(records.size() == 1);

  CHECK(records[0].previous_uid == 1234);
  CHECK(records[0].assigned_uid == 5000);
  CHECK(records[0].kind == UidCollisionKind::M2);
  CHECK(records[0].model_name == "world/wmo/foo.m2");
  CHECK(records[0].tile.known);
  CHECK(records[0].tile.x == 32);
  CHECK(records[0].tile.z == 48);
}

TEST_CASE("the model name is copied, not aliased", "[uid]")
{
  // The caller hands over a view into the FileKey of a Model that the ModelManager can unload as
  // soon as the last instance referencing it goes away. A report opened afterwards would read
  // freed memory if the log kept the view.
  UidCollisionLog log;

  {
    std::string transient ("world/azeroth/doodad.m2");
    log.record(1, 2, UidCollisionKind::M2, transient, knownTile(0, 0));
    transient = "clobbered";
  }

  auto const records = log.snapshot();
  REQUIRE(records.size() == 1);
  CHECK(records[0].model_name == "world/azeroth/doodad.m2");
}

TEST_CASE("an instance with no model records an empty name rather than being dropped", "[uid]")
{
  // The storage passes an empty view when instance_model() returns null. The collision still
  // happened and the uids are still worth showing, so the record must survive without a name.
  UidCollisionLog log;
  log.record(7, 8, UidCollisionKind::WMO, std::string_view(), unknownTile());

  auto const records = log.snapshot();
  REQUIRE(records.size() == 1);
  CHECK(records[0].model_name.empty());
  CHECK(log.totalCount() == 1);
}

TEST_CASE("an out of bounds position is recorded as an unknown tile", "[uid]")
{
  UidCollisionLog log;
  log.record(1, 2, UidCollisionKind::M2, "a.m2", unknownTile());

  auto const records = log.snapshot();
  REQUIRE(records.size() == 1);
  CHECK_FALSE(records[0].tile.known);
}

TEST_CASE("records are kept in the order they were repaired", "[uid]")
{
  // Load order is the only ordering a mapper can correlate against, so it must not be disturbed.
  UidCollisionLog log;

  for (std::uint32_t i = 0; i < 5; ++i)
  {
    log.record(i, 1000 + i, UidCollisionKind::M2, "a.m2", knownTile(i, i));
  }

  auto const records = log.snapshot();
  REQUIRE(records.size() == 5);

  for (std::uint32_t i = 0; i < 5; ++i)
  {
    CHECK(records[i].previous_uid == i);
    CHECK(records[i].assigned_uid == 1000 + i);
  }
}

TEST_CASE("past the cap the total keeps counting and the earliest records are kept", "[uid]")
{
  UidCollisionLog log;

  std::size_t const overflow = 7;
  std::size_t const total = UidCollisionLog::MAX_RECORDS + overflow;

  for (std::size_t i = 0; i < total; ++i)
  {
    log.record(static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(i) + 1
              , UidCollisionKind::M2, "a.m2", knownTile(0, 0));
  }

  CHECK(log.recordedCount() == UidCollisionLog::MAX_RECORDS);
  CHECK(log.totalCount() == total);
  CHECK(log.truncated());

  auto const records = log.snapshot();
  REQUIRE(records.size() == UidCollisionLog::MAX_RECORDS);

  // First kept, not last: the earliest collisions are the ones that identify the desync.
  CHECK(records.front().previous_uid == 0);
  CHECK(records.back().previous_uid == UidCollisionLog::MAX_RECORDS - 1);
}

TEST_CASE("a log that is exactly full is not reported as truncated", "[uid]")
{
  UidCollisionLog log;

  for (std::size_t i = 0; i < UidCollisionLog::MAX_RECORDS; ++i)
  {
    log.record(static_cast<std::uint32_t>(i), 0, UidCollisionKind::M2, "a.m2", unknownTile());
  }

  CHECK(log.recordedCount() == UidCollisionLog::MAX_RECORDS);
  CHECK(log.totalCount() == UidCollisionLog::MAX_RECORDS);
  CHECK_FALSE(log.truncated());
}

TEST_CASE("clearing resets the total as well as the records", "[uid]")
{
  // Clearing only the vector would leave a log that reports collisions it can no longer list,
  // which reads to the UI exactly like a truncated one.
  UidCollisionLog log;

  for (std::size_t i = 0; i < UidCollisionLog::MAX_RECORDS + 3; ++i)
  {
    log.record(static_cast<std::uint32_t>(i), 0, UidCollisionKind::M2, "a.m2", unknownTile());
  }

  log.clear();

  CHECK(log.empty());
  CHECK(log.recordedCount() == 0);
  CHECK(log.totalCount() == 0);
  CHECK_FALSE(log.truncated());
  CHECK(log.snapshot().empty());

  log.record(1, 2, UidCollisionKind::WMO, "b.wmo", knownTile(1, 2));
  CHECK(log.totalCount() == 1);
}

TEST_CASE("a collision formats to one line naming the uids, kind, tile and model", "[uid]")
{
  UidCollisionRecord record;
  record.previous_uid = 1234;
  record.assigned_uid = 5000;
  record.kind = UidCollisionKind::M2;
  record.model_name = "world/foo.m2";
  record.tile = knownTile(32, 48);

  CHECK(formatUidCollision(record) == "uid 1234 -> 5000 (M2, tile 32,48, world/foo.m2)");

  record.kind = UidCollisionKind::WMO;
  record.model_name = "world/bar.wmo";
  CHECK(formatUidCollision(record) == "uid 1234 -> 5000 (WMO, tile 32,48, world/bar.wmo)");
}

TEST_CASE("formatting says so when the tile or the model is not known", "[uid]")
{
  UidCollisionRecord record;
  record.previous_uid = 1;
  record.assigned_uid = 2;
  record.kind = UidCollisionKind::M2;
  record.tile = unknownTile();

  CHECK(formatUidCollision(record) == "uid 1 -> 2 (M2, tile unknown, <no model>)");

  record.model_name = "x.m2";
  CHECK(formatUidCollision(record) == "uid 1 -> 2 (M2, tile unknown, x.m2)");
}

TEST_CASE("concurrent recording loses no collisions", "[uid]")
{
  // Instances load on several threads, so record() is called concurrently while the UI thread can
  // be reading. A lost update here would understate the damage in the report.
  UidCollisionLog log;

  std::size_t const per_thread = 500;
  std::size_t const thread_count = 4;

  std::vector<std::thread> threads;
  threads.reserve(thread_count);

  for (std::size_t t = 0; t < thread_count; ++t)
  {
    threads.emplace_back([&log, per_thread]
    {
      for (std::size_t i = 0; i < per_thread; ++i)
      {
        log.record(static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(i) + 1
                  , UidCollisionKind::M2, "world/concurrent.m2", knownTile(1, 2));
      }
    });
  }

  for (auto& thread : threads)
  {
    thread.join();
  }

  CHECK(log.totalCount() == per_thread * thread_count);
  CHECK(log.recordedCount() == per_thread * thread_count);
}
