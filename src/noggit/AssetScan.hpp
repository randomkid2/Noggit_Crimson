// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_ASSETSCAN_HPP
#define NOGGIT_ASSETSCAN_HPP

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Missing-asset scanning: which models and textures a loaded map references, and which of those
// the client data chain cannot produce.
//
// The module is split in two, and the split runs THROUGH this header rather than between two
// headers, because the pair AssetScan.hpp/AssetScan.cpp is the whole of it:
//
//   - Everything with an out-of-line definition (AssetScanResult, AssetScanner, the path
//     helpers) lives in AssetScan.cpp and depends on nothing but the STL. No Qt, no OpenGL, no
//     BlizzardArchive. That is what lets tests/AssetScanTests.cpp link it on a machine with no
//     client install and no Qt, which is the only way the aggregation logic gets tested at all.
//   - AssetScanCollector, at the bottom, is the world walk. It is a set of templates NOT because
//     it needs to be generic -- there is exactly one World, one Model and one WMO -- but because
//     a template body is not looked up until instantiation. That keeps World.h, Model.h, WMO.h
//     and ClientData.hpp out of this header and out of AssetScan.cpp, so adding AssetScan.cpp to
//     the standalone test target needs no defines, no stubs and no build-system conditionals.
//
// The intended instantiation site is whatever TU owns the menu action; see collectWorld for the
// exact call.
namespace Noggit
{
  // What a reference points at. Classification is by file extension only -- see
  // assetKindFromPath for why nothing better is available at this layer.
  enum class AssetKind
  {
    Model,       // .m2, and the .mdx/.mdl spellings that still name one
    WorldModel,  // .wmo
    Texture,     // .blp
    Other        // anything else; kept rather than dropped, see AssetScanResult::addReference
  };

  // Verdict for one asset, as determined by the caller's existence probe.
  enum class AssetStatus
  {
    Present,
    Missing,
    // The file is in the chain but the loader rejected it. A separate state because the fix is
    // different: Missing means "ship the file", Unreadable means "the file you shipped is
    // corrupt, truncated, or a format this client cannot parse". Both render as nothing in game,
    // which is exactly why collapsing them into one bucket wastes the reporter's time.
    Unreadable
  };

  // Number of distinct assets in one kind, split by verdict. Distinct, not references: a tileset
  // texture used by all 256 chunks of an ADT is one asset here and 256 in totalReferences().
  struct AssetKindCounts
  {
    std::size_t referenced = 0;
    std::size_t missing = 0;
    std::size_t unreadable = 0;
  };

  // One asset that failed, with enough context to act on it.
  struct MissingAsset
  {
    // Normalised form, and what dedup and lookup key on.
    std::string key;
    // The first spelling encountered, preserved because that is the string actually written in
    // the ADT/DBC the reporter has to go and fix. Reporting only the normalised key would send
    // them looking for a string that appears nowhere in the data.
    std::string display_path;
    AssetKind kind = AssetKind::Other;
    AssetStatus status = AssetStatus::Missing;
    std::size_t reference_count = 0;
    // Bounded by AssetScanOptions::max_referrers_per_asset. A missing tileset texture is
    // referenced by every chunk that paints it; storing all of them turns a 3-line report into a
    // 40000-line one and tells the reader nothing the count did not.
    std::vector<std::string> referrers;
  };

  // Human-readable name, stable enough to appear in a report and in test expectations.
  std::string_view assetKindName(AssetKind kind);

  // Classifies by extension.
  //
  // Extension is all there is: at the point a reference is recorded it is a string out of an ADT
  // block, a DBC string table or an M2 texture list, and nothing has tried to open it yet. A
  // .mdx or .mdl name classifies as Model because that is what it names -- the extension rewrite
  // to .m2 belongs to Database::ModelPathFixup and to the archive layer, not here. The scanner
  // must report the string the map really contains.
  AssetKind assetKindFromPath(std::string_view path);

  // Canonical key for one asset path: trimmed, ASCII-lowercased, separators normalised to '\\',
  // separator runs collapsed, leading separators dropped.
  //
  // Returns an EMPTY STRING for input that cannot be a path (empty, or whitespace and separators
  // only), which is how callers detect a reference worth rejecting rather than probing.
  //
  // Lowercasing is done with an explicit ASCII range test rather than ::tolower. ::tolower takes
  // an int and has undefined behaviour for negative values, which is every byte >= 0x80 once a
  // signed char is promoted -- and ADT texture lists and DBC string tables do carry such bytes.
  // ClientData::normalizeFilenameInternal (ClientData.cpp:669) has exactly that bug; this does
  // not reproduce it.
  //
  // NOTE this deliberately does NOT rewrite .mdx/.mdl to .m2. The consequence is that a map
  // referencing both spellings of one model counts them as two assets and costs two probes. That
  // is the cheaper error: rewriting here would make the report name a file the data does not.
  std::string normalizeAssetKey(std::string_view path);

  // Pure aggregation over recorded references. Holds no probe, no client data and no world.
  //
  // Split out from AssetScanner so the interesting half -- dedup, per-kind bookkeeping, referrer
  // capping, ordering, merging -- is exercised by tests/AssetScanTests.cpp with hand-built input
  // and no client install anywhere near it.
  class AssetScanResult
  {
  public:
    // Cap on referrers retained per asset when the caller does not specify one.
    static constexpr std::size_t DEFAULT_MAX_REFERRERS = 8;

    // Records one reference and its verdict.
    //
    // Repeated calls for the same asset accumulate reference_count and (up to the cap) referrers.
    // The verdict of the FIRST call wins: the probe is memoised per asset upstream, so a
    // disagreeing later call means the chain changed underneath the scan, and silently adopting
    // the newer answer would make the totals inconsistent with the per-kind counts already
    // emitted. Use disagreementCount() to detect that this happened.
    //
    // Returns false and increments rejectedCount() when `path` does not normalise to anything.
    bool addReference( AssetKind kind
                     , std::string_view path
                     , std::string_view referrer
                     , AssetStatus status
                     , std::size_t max_referrers = DEFAULT_MAX_REFERRERS
                     );

    // Accepted addReference calls, i.e. reference sites, not distinct assets.
    std::size_t totalReferences() const;
    std::size_t distinctReferenced() const;
    std::size_t distinctMissing() const;
    std::size_t distinctUnreadable() const;
    // Calls rejected because the path did not normalise.
    std::size_t rejectedCount() const;
    // Times a later reference to a known asset arrived with a different verdict. Non-zero means
    // the report is a snapshot of a chain that moved, not that the scan is wrong.
    std::size_t disagreementCount() const;

    AssetKindCounts counts(AssetKind kind) const;

    // True when nothing at all was recorded. Distinct from "nothing missing".
    bool empty() const;
    bool hasFailures() const;

    // Failures in report order: by kind (Model, WorldModel, Texture, Other), then by descending
    // reference count, then by key. The ordering is total and deterministic so two scans of the
    // same map produce byte-identical reports and a diff between them means something really
    // changed.
    std::vector<MissingAsset> failures() const;
    std::vector<MissingAsset> failures(AssetKind kind) const;

    // Lookup by any spelling of the path; normalisation is applied for you.
    bool wasReferenced(std::string_view path) const;
    AssetStatus statusOf(std::string_view path, AssetStatus fallback = AssetStatus::Present) const;
    std::size_t referenceCount(std::string_view path) const;

    // Folds another result in. Reference counts add, referrers merge up to the cap, and a verdict
    // conflict is counted as a disagreement with this result's verdict kept.
    //
    // Exists because the world walk is naturally per-tile and a caller may want to scan tiles on
    // separate threads, or scan, unload, load more and scan again. Without merge that caller has
    // to reimplement the dedup, which is the part worth having only one copy of.
    void merge(AssetScanResult const& other, std::size_t max_referrers = DEFAULT_MAX_REFERRERS);

    // One line, suitable for a status bar.
    std::string summary() const;

    // Full plain-text report. Deterministic; safe to write to a file and diff.
    std::string toReport() const;

  private:
    struct Entry
    {
      std::string display_path;
      AssetKind kind = AssetKind::Other;
      AssetStatus status = AssetStatus::Present;
      std::size_t reference_count = 0;
      std::vector<std::string> referrers;
    };

    void mergeEntry(std::string const& key, Entry const& entry, std::size_t max_referrers);

    std::unordered_map<std::string, Entry> _entries;
    std::size_t _total_references = 0;
    std::size_t _rejected = 0;
    std::size_t _disagreements = 0;
  };

  struct AssetScanOptions
  {
    std::size_t max_referrers_per_asset = AssetScanResult::DEFAULT_MAX_REFERRERS;
    // Walk the texture list inside each loaded M2. Off makes the scan cheap and terrain-only.
    bool scan_model_textures = true;
    // Walk each loaded WMO's own textures and its doodad model names.
    bool scan_world_model_contents = true;
    // Report the .blp names painted on terrain, read from each chunk's texture set.
    bool scan_terrain_textures = true;
  };

  // Drives an existence probe and accumulates the answers.
  //
  // The probe is injected as a std::function<bool(std::string const&)> rather than a ClientData&
  // so this class links no archive code. AssetScanCollector::makeClientDataProbe builds the real
  // one; tests build a table lookup.
  //
  // Memoisation is the reason this is a class and not a free function. A map's chunk texture
  // lists resolve to a few hundred distinct .blp names across tens of thousands of references,
  // and ClientData::exists walks every open archive on a miss (ClientData.cpp:575) after a
  // filesystem stat. Probing once per distinct asset instead of once per reference is the
  // difference between a scan that finishes and one that appears to hang.
  class AssetScanner
  {
  public:
    using ExistsFn = std::function<bool (std::string const&)>;

    explicit AssetScanner(ExistsFn exists_fn, AssetScanOptions options = {});

    // Records one reference, probing the chain if this asset has not been seen before.
    //
    // `already_failed_to_load` reports what the loader already knows: an AsyncObject that has
    // finished loading with loading_failed() set is either Missing (the probe says the file is
    // not there) or Unreadable (the probe finds it, so the loader rejected its contents).
    // Callers with no loader state pass false.
    void addReference( AssetKind kind
                     , std::string_view path
                     , std::string_view referrer
                     , bool already_failed_to_load = false
                     );

    // Same, classifying by extension.
    void addReference(std::string_view path, std::string_view referrer, bool already_failed_to_load = false);

    AssetScanResult const& result() const;
    AssetScanResult takeResult();

    AssetScanOptions const& options() const;

    // Distinct probes actually issued. Only interesting as evidence that memoisation works,
    // which is what its test asserts.
    std::size_t probeCount() const;

  private:
    ExistsFn _exists_fn;
    AssetScanOptions _options;
    AssetScanResult _result;
    std::unordered_map<std::string, bool> _probe_cache;
    std::size_t _probe_count = 0;
  };

  // The world walk. Templates only so this header pulls in no Noggit or archive headers; see the
  // note at the top of the file.
  namespace AssetScanCollector
  {
    // Wraps a BlizzardArchive::ClientData in an AssetScanner::ExistsFn.
    //
    // Two things the raw call does not do:
    //
    //   - Catches everything. ClientData::exists itself does not throw, but it begins with
    //     existsOnDisk -> fs::exists(getDiskPath(...)) (ClientData.cpp:567), and the throwing
    //     overload of std::filesystem::exists reports errors other than "not found" by throwing
    //     filesystem_error. A project directory on a disconnected network share is enough. An
    //     asset scan must never take the editor down, and a probe that cannot answer is reported
    //     as Missing, which is the conservative direction: it produces a line to investigate
    //     rather than silently clearing a file.
    //   - Takes a non-const pointer, because exists() is non-const and locks ClientData's own
    //     mutex. Do not call the returned function from a thread already holding it.
    //
    // The std::string converts to a Listfile::FileKey through FileKey's non-explicit constructor
    // (Listfile.hpp:38), which is also where the path gets lowercased and its separators flipped.
    // AssetScanner hands this function the normalizeAssetKey form rather than the raw spelling,
    // which matters for one case: a path with a LEADING separator. FileKey keeps it, and
    // getDiskPath then builds `fs::path(project_dir) / "/world/foo.m2"` (ClientData.cpp:636) --
    // where operator/ discards the left operand for a rooted right one, so the disk probe checks
    // a path on the wrong drive root. normalizeAssetKey drops the leading separator, which is
    // also how the listfile spells it.
    template <typename ClientDataT>
    AssetScanner::ExistsFn makeClientDataProbe(ClientDataT* client_data)
    {
      return [client_data] (std::string const& path) -> bool
      {
        if (!client_data)
        {
          return false;
        }

        try
        {
          return client_data->exists(path);
        }
        catch (...)
        {
          return false;
        }
      };
    }

    // The texture list inside one loaded M2.
    //
    // Slots the loader substituted are skipped. For every texture whose M2 `type` is non-zero --
    // the runtime-supplied skins -- Model.cpp:362 overwrites the stored name with the constant
    // "tileset/generic/black.blp" and flattens _specialTextures to -1. That name is Noggit's
    // placeholder, not a reference the map makes, and reporting it would attribute an editor
    // constant to the map's data. _replaceable_texture_types (Model.h:230) is the only surviving
    // record of which slots those were, which is what it was added for.
    template <typename ModelT>
    void collectModelTextures(ModelT* model, std::string const& referrer, AssetScanner& scanner)
    {
      auto const& filenames = model->_textureFilenames;
      auto const& replaceable_types = model->_replaceable_texture_types;

      for (std::size_t slot = 0; slot < filenames.size(); ++slot)
      {
        if (slot < replaceable_types.size() && replaceable_types[slot] != 0)
        {
          continue;
        }

        // A type-0 slot with a zero-length name is skipped by the loader (Model.cpp:346), leaving
        // the empty string it was resized to.
        if (filenames[slot].empty())
        {
          continue;
        }

        scanner.addReference(AssetKind::Texture, filenames[slot], referrer);
      }
    }

    // One loaded WMO's own textures (MOTX/MOMT) and the M2s of its doodad sets (MODN/MODD).
    //
    // The doodads are the reason this exists: an ADT names the .wmo and nothing else, so a
    // missing doodad model or a missing WMO texture is invisible to a walk that only reads the
    // tile's object list, and it is exactly the case that renders as a hole in a building.
    template <typename ModelT, typename WorldModelT>
    void collectWorldModelContents(WorldModelT* world_model, std::string const& referrer, AssetScanner& scanner)
    {
      for (auto const& texture : world_model->textures)
      {
        auto const* blp = texture.get();

        if (!blp)
        {
          continue;
        }

        auto const& texture_key = blp->file_key();

        if (!texture_key.hasFilepath())
        {
          continue;
        }

        scanner.addReference( AssetKind::Texture
                            , texture_key.filepath()
                            , referrer
                            , blp->finishedLoading() && blp->loading_failed()
                            );
      }

      // `modelis`, not the `models` member next to it: WMO::models (WMO.h:325) is declared and
      // never written -- no assignment to it exists anywhere in WMO.cpp. The MODN name table is
      // consumed straight into wmo_doodad_instance objects at WMO.cpp:318, so the instances are
      // the only place the doodad file names survive.
      for (auto const& doodad : world_model->modelis)
      {
        auto* doodad_object = doodad.instance_model();

        if (!doodad_object)
        {
          continue;
        }

        auto const& doodad_key = doodad_object->file_key();

        if (!doodad_key.hasFilepath())
        {
          continue;
        }

        std::string const& doodad_path = doodad_key.filepath();
        bool const doodad_load_failed = doodad_object->finishedLoading() && doodad_object->loading_failed();

        scanner.addReference(AssetKind::Model, doodad_path, referrer, doodad_load_failed);

        if (scanner.options().scan_model_textures && !doodad_load_failed && doodad_object->finishedLoading())
        {
          if (auto* doodad_model = dynamic_cast<ModelT*>(doodad_object))
          {
            collectModelTextures(doodad_model, doodad_path, scanner);
          }
        }
      }
    }

    // Records everything one loaded tile references.
    //
    // Exposed separately from collectWorld so a caller can scan incrementally, or in parallel
    // with a scanner per thread whose results are merged afterwards via AssetScanResult::merge.
    //
    // TileT is MapTile, ModelT is Model, WorldModelT is WMO. They are template parameters purely
    // to keep their headers out of this one.
    template <typename TileT, typename ModelT, typename WorldModelT>
    void collectTile(TileT* tile, AssetScanner& scanner)
    {
      // finishedLoading() is checked even though MapIndex::loaded_tiles() already filters on it
      // (map_index.cpp:29), because collectTile is also callable directly with a tile the caller
      // got some other way, and reading a half-parsed tile's chunk array is a data race.
      if (!tile || !tile->finishedLoading())
      {
        return;
      }

      std::string const tile_referrer
        ("ADT " + std::to_string(tile->index.x) + "," + std::to_string(tile->index.z));

      if (scanner.options().scan_terrain_textures)
      {
        // 16x16 rather than MapTile's own iterator because it has none -- MapTile.h:84 carries a
        // "\todo map_index style iterators" for precisely this. World::for_all_chunks_on_tile is
        // the existing helper and is unusable here: it calls mapIndex.setChanged(tile) on entry
        // (World.inl:16), so using it would mark every visited tile dirty and put unsaved-change
        // prompts in front of a user who only asked a question.
        for (unsigned int chunk_z = 0; chunk_z < 16; ++chunk_z)
        {
          for (unsigned int chunk_x = 0; chunk_x < 16; ++chunk_x)
          {
            auto* chunk = tile->getChunk(chunk_x, chunk_z);

            if (!chunk)
            {
              continue;
            }

            auto* texture_set = chunk->getTextureSet();

            if (!texture_set)
            {
              continue;
            }

            for (std::size_t layer = 0; layer < texture_set->num(); ++layer)
            {
              scanner.addReference(AssetKind::Texture, texture_set->filename(layer), tile_referrer);
            }
          }
        }
      }

      // The keys of this map are the shared Model/WMO objects, one entry per distinct file on the
      // tile, with every instance of it in the value. Iterating the keys is therefore already
      // deduplicated per tile, and -- the point of the whole exercise -- an entry exists even
      // when the file is absent, because the object is created from the ADT's MMDX/MWMO name
      // before any load is attempted and simply ends up with loading_failed() set.
      for (auto const& instances : tile->getObjectInstances())
      {
        auto* object = instances.first;

        if (!object)
        {
          continue;
        }

        auto const& object_key = object->file_key();

        if (!object_key.hasFilepath())
        {
          continue;
        }

        std::string const& object_path = object_key.filepath();
        bool const object_load_failed = object->finishedLoading() && object->loading_failed();

        scanner.addReference(object_path, tile_referrer, object_load_failed);

        // Contents are only populated by finishLoading(). Reading them earlier races the async
        // loader; reading them after a failed load yields nothing useful. Either way the
        // reference to the object itself, recorded above, is what matters.
        if (!object->finishedLoading() || object->loading_failed())
        {
          continue;
        }

        if (scanner.options().scan_model_textures)
        {
          if (auto* model = dynamic_cast<ModelT*>(object))
          {
            collectModelTextures(model, object_path, scanner);
          }
        }

        if (scanner.options().scan_world_model_contents)
        {
          if (auto* world_model = dynamic_cast<WorldModelT*>(object))
          {
            collectWorldModelContents<ModelT, WorldModelT>(world_model, object_path, scanner);
          }
        }
      }
    }

    // Walks every loaded tile of `world`.
    //
    // Call as:
    //
    //   Noggit::AssetScanner scanner (Noggit::AssetScanCollector::makeClientDataProbe(client_data));
    //   Noggit::AssetScanCollector::collectWorld<World, MapTile, Model, WMO>(*world, scanner);
    //
    // from a TU that has included World.h, MapTile.h, MapChunk.h, texture_set.hpp, Model.h and
    // WMO.h. None of the four type parameters can be deduced from `world` alone, so they are
    // always written out.
    //
    // LOADED tiles only, via MapIndex::loaded_tiles(), which already filters to non-null and
    // finishedLoading() (map_index.cpp:29). Tiles the user has never visited are not scanned.
    // Pulling them in would mean loading the whole map, which is a different operation with a
    // different cost and belongs behind its own explicit action.
    template <typename WorldT, typename TileT, typename ModelT, typename WorldModelT>
    void collectWorld(WorldT& world, AssetScanner& scanner)
    {
      for (TileT* tile : world.mapIndex.loaded_tiles())
      {
        collectTile<TileT, ModelT, WorldModelT>(tile, scanner);
      }
    }
  }
}

#endif // NOGGIT_ASSETSCAN_HPP
