// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_PATCHASSETPACKER_HPP
#define NOGGIT_PATCHASSETPACKER_HPP

#include <noggit/AssetDependencies.hpp>
#include <noggit/AssetScan.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace BlizzardArchive
{
  class ClientData;

  namespace Archive
  {
    class MPQArchive;
  }
}

// Packs the assets the project's terrain REFERENCES but does not contain.
//
// Client > Patch Client writes the project folder into an MPQ patch
// (ClientData::saveLocalFilesToArchive) and nothing else. A WMO or M2 placed from another patch or
// from the base client is named by the ADT and absent from the patch, so it is missing in game.
// This walks from every ADT in the project folder out through the whole reference chain, works out
// where each file resolves from, and adds the ones that are not already somewhere the player has.
//
// WHY IT LIVES IN src/noggit AND NOT IN ClientData
//
// ClientData::_mutex is a plain std::mutex held for the ENTIRE duration of saveLocalFilesToArchive
// (ClientData.cpp:454). readFile, exists and getDiskPath all take it. Adding the dependency pass
// inside saveLocalFilesToArchive -- the obvious place -- self-deadlocks on the first ClientFile it
// constructs. It has to be a separate pass that holds no ClientData lock, and a separate pass can
// live entirely in src/noggit. That also keeps it visible to anyone who clones this repository:
// blizzard-archive-library is a submodule pointing at a third party, so nothing added there ships.
//
// The consequence is TWO PHASES, and they are not an implementation detail:
//
//   1. Read. Every dependency is resolved and extracted to a staging directory. Reads only; the
//      target archive stays read-only and the background model loader can keep using it.
//   2. Write. The archive is opened for writing once and every staged file is added from its disk
//      path through MPQArchive::addFile, which is the writer that carries the backslash fix.
//
// Doing it in one pass would mean SFileAddFileEx on a handle another thread may be reading through.
namespace Noggit
{
  // Where a reference resolved from. The order of resolution is ClientFile's: the project folder
  // first (ClientFile.cpp:22-36), then the loaded archives in reverse priority
  // (ClientData.cpp:419).
  enum class AssetOrigin
  {
    // Nothing in the chain has it. This is what gets NAMED in the report.
    NotFound,
    // In the project folder, so the existing recursive_directory_iterator pass already packs it.
    ProjectFolder,
    // Already inside the patch being written, from an earlier run.
    TargetArchive,
    // In a stock 3.3.5a archive: every player already has these bytes.
    BaseClientArchive,
    // In some other patch -- the user's own content, or another mod's.
    CustomArchive
  };

  std::string_view assetOriginName(AssetOrigin origin);

  struct PatchAssetPackerOptions
  {
    // Off by default, and deliberately so. A stock building references stock textures; including
    // them turns a small patch into a multi-gigabyte copy of files every player already has. The
    // conservative direction for a packer is the one that does not silently multiply the size of
    // the thing being shipped. Turning it on is for building a self-contained archive, which is a
    // different job.
    bool include_base_client_assets = false;

    // Passed through to MPQArchive::addFile. Roughly 3x slower, much smaller.
    bool compress = true;

    // Run SFileCompactArchive after the added files. Only meaningful when something was added.
    bool compact = false;
  };

  struct PatchAssetPackerResult
  {
    // Files in the project folder the walk started from.
    std::size_t roots = 0;
    // Distinct references visited, roots included.
    std::size_t visited = 0;
    std::size_t added = 0;
    std::size_t skipped_in_project = 0;
    std::size_t skipped_base_client = 0;
    std::size_t skipped_already_in_patch = 0;
    // Optional references that are simply not there: external .anim files, LOD skins above 00, and
    // tileset specular companions. Counted so the numbers in the summary ADD UP -- without it a
    // user sees "469 references" against a handful of tallies and has no way to tell whether the
    // difference is a category or a bug.
    std::size_t optional_absent = 0;
    std::size_t write_failures = 0;
    std::uint64_t bytes_added = 0;
    bool cancelled = false;

    // Every write that threw or returned false, with the archive's own message.
    std::vector<std::string> write_errors;

    // Dedup, per-kind counts, referrers and deterministic ordering, all from AssetScan. Required
    // references that resolved nowhere are AssetStatus::Missing here, and are what failures()
    // names.
    AssetScanResult scan;

    std::string summary() const;
    // Deterministic; safe to write to a file and diff between runs.
    std::string toReport() const;
  };

  class PatchAssetPacker
  {
  public:
    // (phase, done, total, current path) -> false to cancel.
    //
    // `total` GROWS during the scan phase: the walk is breadth-first and the size of the graph is
    // not known until it has been walked. A caller driving a QProgressDialog should set the
    // maximum on every call.
    //
    // Called only BETWEEN assets, never while a ClientFile or an archive handle is open, because a
    // Qt implementation of this pumps the event loop.
    using ProgressFn = std::function<bool (char const*, std::size_t, std::size_t, std::string const&)>;

    // `target_archive` must be the archive Patch Client is writing, already created. Neither
    // pointer is owned; both are owned by the ClientData singleton and outlive any one pack.
    PatchAssetPacker( BlizzardArchive::ClientData* client_data
                    , BlizzardArchive::Archive::MPQArchive* target_archive
                    , PatchAssetPackerOptions options
                    );

    // Removes the staging directory. Runs even after an exception escaped run().
    ~PatchAssetPacker();

    PatchAssetPacker(PatchAssetPacker const&) = delete;
    PatchAssetPacker& operator= (PatchAssetPacker const&) = delete;

    PatchAssetPackerResult run(ProgressFn const& progress);

  private:
    struct StagedFile
    {
      // Backslashes, lowercase: byte-for-byte what the client will hash for.
      std::string stored_name;
      std::filesystem::path disk_path;
      std::uint64_t size = 0;
    };

    struct QueuedReference
    {
      std::string path;
      std::string referrer;
      AssetKind kind = AssetKind::Other;
      ReferenceNecessity necessity = ReferenceNecessity::Required;
      bool traversable = true;
      // Roots are project files that are packed by the existing pass; they are walked but not
      // counted as dependencies and never staged.
      bool is_root = false;
    };

    void collectRoots(std::vector<QueuedReference>& queue, PatchAssetPackerResult& result) const;
    AssetOrigin originOf(std::string const& path) const;
    // Creates the staging directory on first use, so a pack that stages nothing leaves no trace.
    std::filesystem::path nextStagingPath();
    void removeStagingDirectory();

    BlizzardArchive::ClientData* _client_data;
    BlizzardArchive::Archive::MPQArchive* _target_archive;
    PatchAssetPackerOptions _options;

    std::filesystem::path _staging_directory;
    bool _staging_created = false;
    std::size_t _staging_counter = 0;
    std::vector<StagedFile> _staged;
  };
}

#endif // NOGGIT_PATCHASSETPACKER_HPP
