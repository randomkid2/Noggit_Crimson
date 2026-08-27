// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/assets/PatchAssetPacker.hpp>

#include <noggit/assets/AssetDependencies.hpp>
#include <noggit/Log.h>

#include <BaseArchive.hpp>
#include <ClientData.hpp>
#include <ClientFile.hpp>
#include <Exception.hpp>
#include <Listfile.hpp>
#include <MPQArchive.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace
{
  // Files whose contents name other files. Everything else is a leaf and only has to be copied.
  //
  // .wdt is here with .adt because the two agree on the blocks this reads: a WDT for a global-WMO
  // map carries MWMO/MODF with the same NUL separated string block shape, so one parser covers
  // both and a map whose only object is its global WMO is not silently skipped.
  bool isTraversable(std::string const& path)
  {
    auto const has_extension
      ( [&path] (char const* extension)
        {
          std::size_t const length = std::strlen(extension);
          return path.size() > length && path.compare(path.size() - length, length, extension) == 0;
        }
      );

    return has_extension(".adt") || has_extension(".wdt")
        || has_extension(".m2") || has_extension(".wmo");
  }

  bool isAdtLike(std::string const& path)
  {
    return path.size() > 4
        && (path.compare(path.size() - 4, 4, ".adt") == 0 || path.compare(path.size() - 4, 4, ".wdt") == 0);
  }

  bool isWorldModel(std::string const& path)
  {
    return path.size() > 4 && path.compare(path.size() - 4, 4, ".wmo") == 0;
  }

  bool isModel(std::string const& path)
  {
    return path.size() > 3 && path.compare(path.size() - 3, 3, ".m2") == 0;
  }

  // Dispatches to whichever parser matches the extension, and RETURNS what it said.
  //
  // The three calls used to sit inline in run() with their results dropped, which is what made a
  // truncated or malformed file indistinguishable from one that legitimately references nothing:
  // the status had already been recorded as Present, the child list came back empty, and the file
  // was counted as a successfully walked reference in a summary that named it nowhere.
  Noggit::AssetParseResult parseReferences( std::string const& path
                                          , std::vector<char> const& buffer
                                          , std::vector<Noggit::AssetReference>& out
                                          )
  {
    if (isAdtLike(path))
    {
      return Noggit::collectAdtReferences(buffer.data(), buffer.size(), out);
    }

    if (isModel(path))
    {
      return Noggit::collectModelReferences(path, buffer.data(), buffer.size(), out);
    }

    if (isWorldModel(path))
    {
      return Noggit::collectWorldModelReferences(path, buffer.data(), buffer.size(), out);
    }

    // isTraversable() gates the call, so nothing reaches this. Complete rather than Unrecognised so
    // that adding an extension to that list and forgetting a parser here is a silent no-op instead
    // of a report full of parse failures for files that were never meant to be parsed.
    return { Noggit::ParseOutcome::Complete, {} };
  }

  // One recorded reference EDGE: a (referrer, target) pair, kept whether or not the target had
  // already been seen.
  //
  // The walk queues each target once -- it has to, a WMO naming its own doodads is a cycle -- and
  // it used to record the reference at DEQUEUE time, so every asset was credited with exactly one
  // reference however many files named it. A texture used by forty models read as "(1 reference)",
  // which is the number that decides whether a missing file is worth chasing first.
  //
  // POINTERS into an interning pool, not strings. There is one edge per reference site rather than
  // per asset -- a project with a couple of thousand ADTs produces high hundreds of thousands of
  // them -- and two std::strings each, both past the small-string limit at typical path lengths,
  // is over a hundred megabytes of duplicated text for a list that is read once.
  struct ReferenceEdge
  {
    std::string const* path = nullptr;
    std::string const* referrer = nullptr;
    Noggit::AssetKind kind = Noggit::AssetKind::Other;
    Noggit::ReferenceNecessity necessity = Noggit::ReferenceNecessity::Required;
  };

  // What the walk decided about one path, once.
  struct PathVerdict
  {
    Noggit::AssetStatus status = Noggit::AssetStatus::Present;
    // Nothing in the chain has it. Held apart from `status` because an absent reference is recorded
    // only when it is REQUIRED, and necessity belongs to the edge while the probe belongs to the
    // path -- the same file can be optional to one parent and required to another.
    bool absent = false;
  };
}

std::string_view Noggit::assetOriginName(AssetOrigin origin)
{
  switch (origin)
  {
    case AssetOrigin::NotFound:          return "not found";
    case AssetOrigin::ProjectFolder:     return "project folder";
    case AssetOrigin::TargetArchive:     return "already in this patch";
    case AssetOrigin::BaseClientArchive: return "base client";
    case AssetOrigin::CustomArchive:     return "other patch";
  }

  return "not found";
}

std::string Noggit::PatchAssetPackerResult::summary() const
{
  std::ostringstream line;

  line << "Walked " << roots << " project file(s) and " << visited << " reference(s): "
       << added << " added";

  if (bytes_added)
  {
    line << " (" << (bytes_added / 1024u) << " KiB)";
  }

  line << ", " << skipped_base_client << " already in the base client"
       << ", " << skipped_in_project << " already in the project"
       << ", " << skipped_already_in_patch << " already in this patch"
       << ", " << optional_absent << " optional files absent (anims, LOD skins, specular maps)"
       << ", " << scan.distinctMissing() << " unresolved";

  if (scan.distinctUnreadable())
  {
    // Kept apart from unresolved because the fix differs: a missing file has to be shipped, an
    // unreadable one has to be replaced. Both render as nothing in game, which is exactly why
    // collapsing them wastes the reader's time.
    line << ", " << scan.distinctUnreadable() << " unreadable";
  }

  if (!parse_failures.empty())
  {
    // Shouted, because it is the only number here that means the OTHER numbers are incomplete.
    //
    // Deliberately says SOME rather than none. A partial parse -- forEachChunk stopping at a bad
    // chunk header -- yields the references it reached and loses the rest, so a blanket "never
    // followed" would be false for exactly the files most likely to occur, and a report that
    // overstates the damage gets distrusted as fast as one that understates it. The per-file
    // detail carries the recovered count and where it stopped.
    line << ", " << parse_failures.size()
         << " UNPARSED OR PARTIAL (some of their own references were never followed)";
  }

  if (write_failures)
  {
    line << ", " << write_failures << " failed to write";
  }

  if (cancelled)
  {
    line << " (CANCELLED)";
  }

  return line.str();
}

std::string Noggit::PatchAssetPackerResult::toReport() const
{
  std::ostringstream report;

  report << summary() << "\n";

  // FIRST, above the missing and unreadable lists, because it is the section that says those lists
  // are not the whole story. A file that did not parse contributed no references at all, so the
  // things it names are absent from every other section here by construction.
  if (!parse_failures.empty())
  {
    report << "\nFiles that were read but whose contents could not be fully parsed.\n"
              "Everything THESE files reference was never followed and is not in this patch.\n"
              "No other line in this report can show those names -- they were never looked up.\n\n";

    for (auto const& failure : parse_failures)
    {
      report << "  " << (failure.partial ? "PARTIAL" : "FAILED ")
             << "  " << failure.path << "\n"
             << "      " << failure.reason << "\n";

      if (failure.partial)
      {
        report << "      " << failure.recovered
               << " reference(s) were recovered before the walk stopped; the rest were not\n";
      }

      report << "      referenced by " << failure.referrer << "\n";
    }
  }

  if (scan.hasFailures())
  {
    // AssetScanResult::failures() returns Missing AND Unreadable together, and the two need
    // different sentences: ship the file versus replace the corrupt one. Printing them under one
    // heading that says "could not be resolved" mislabels every zero byte or truncated file as an
    // absent one and sends the reader looking for something that is right there.
    auto const heading
      ( [&report] (AssetStatus status)
        {
          if (status == AssetStatus::Missing)
          {
            report << "\nReferences that could not be resolved anywhere in the client data chain.\n"
                      "These will be missing in game; nothing was packed for them.\n\n";
          }
          else
          {
            report << "\nReferences that resolved but could not be read: empty, truncated, or a\n"
                      "format the chain rejected. The file is there and its contents are not.\n\n";
          }
        }
      );

    for (AssetStatus const status : { AssetStatus::Missing, AssetStatus::Unreadable })
    {
      bool printed_heading = false;

      // Ordered by kind, then by descending reference count, then by key, so two runs over an
      // unchanged project produce byte-identical text and a diff means something really changed.
      for (auto const& failure : scan.failures())
      {
        if (failure.status != status)
        {
          continue;
        }

        if (!printed_heading)
        {
          heading(status);
          printed_heading = true;
        }

        report << "  " << assetKindName(failure.kind) << "  " << failure.display_path
               << "  (" << failure.reference_count << " reference"
               << (failure.reference_count == 1 ? "" : "s") << ")\n";

        for (auto const& referrer : failure.referrers)
        {
          report << "      referenced by " << referrer << "\n";
        }
      }
    }
  }

  if (!write_errors.empty())
  {
    report << "\nFiles that resolved but could not be written to the archive.\n\n";

    for (auto const& error : write_errors)
    {
      report << "  " << error << "\n";
    }
  }

  return report.str();
}

Noggit::PatchAssetPacker::PatchAssetPacker( BlizzardArchive::ClientData* client_data
                                          , BlizzardArchive::Archive::MPQArchive* target_archive
                                          , PatchAssetPackerOptions options
                                          )
  : _client_data (client_data)
  , _target_archive (target_archive)
  , _options (options)
{
}

Noggit::PatchAssetPacker::~PatchAssetPacker()
{
  removeStagingDirectory();
}

void Noggit::PatchAssetPacker::removeStagingDirectory()
{
  if (!_staging_created)
  {
    return;
  }

  std::error_code error;
  fs::remove_all(_staging_directory, error);

  if (error)
  {
    LogError << "PatchAssetPacker: could not remove staging directory "
             << _staging_directory.string() << ": " << error.message() << std::endl;
  }

  _staging_created = false;
}

std::filesystem::path Noggit::PatchAssetPacker::nextStagingPath()
{
  if (!_staging_created)
  {
    // Numbered files in one flat directory rather than a mirror of the asset tree. The staged name
    // never reaches the archive -- addFile takes the stored name separately -- so nothing is gained
    // by recreating the paths, and a flat directory cannot hit MAX_PATH or fail to create an
    // intermediate directory halfway through a pack.
    auto const stamp
      (std::chrono::steady_clock::now().time_since_epoch().count());

    _staging_directory
      = fs::temp_directory_path() / ("noggit-patch-deps-" + std::to_string(static_cast<std::uint64_t>(stamp)));

    fs::create_directories(_staging_directory);
    _staging_created = true;
  }

  return _staging_directory / (std::to_string(_staging_counter++) + ".bin");
}

Noggit::AssetOrigin Noggit::PatchAssetPacker::originOf(std::string const& path) const
{
  BlizzardArchive::Listfile::FileKey const key (path);

  try
  {
    // Disk first, exactly as ClientFile does. existsOnDisk goes through the throwing overload of
    // std::filesystem::exists (ClientData.cpp:582), which raises for anything other than "not
    // found" -- a disconnected network share is enough.
    if (_client_data->existsOnDisk(key))
    {
      return AssetOrigin::ProjectFolder;
    }
  }
  catch (...)
  {
  }

  auto const* archives = _client_data->loadedArchives();

  if (!archives)
  {
    return AssetOrigin::NotFound;
  }

  // Reverse, because ClientData::readFile resolves in reverse and the LAST loaded archive wins.
  // Asking any other one first would attribute the file to an archive the client will not read it
  // from.
  for (auto it = archives->rbegin(); it != archives->rend(); ++it)
  {
    bool present = false;

    try
    {
      present = (*it)->exists(key, _client_data->locale_mode());
    }
    catch (...)
    {
      continue;
    }

    if (!present)
    {
      continue;
    }

    if (_target_archive && (*it)->path() == _target_archive->path())
    {
      return AssetOrigin::TargetArchive;
    }

    return isBaseClientArchiveName((*it)->path())
      ? AssetOrigin::BaseClientArchive
      : AssetOrigin::CustomArchive;
  }

  return AssetOrigin::NotFound;
}

void Noggit::PatchAssetPacker::collectRoots( std::vector<QueuedReference>& queue
                                           , PatchAssetPackerResult& result
                                           ) const
{
  std::string const project_path (_client_data->projectPath());

  std::error_code error;

  if (!fs::is_directory(project_path, error) || error)
  {
    return;
  }

  // Every ADT and WDT in the project is a root, whether or not the user ever loaded that tile.
  // That is the structural reason AssetScanCollector cannot be used here: it walks
  // MapIndex::loaded_tiles(), and a project folder routinely holds tiles nobody visited this
  // session.
  //
  // M2 and WMO files in the project are roots too. They are already packed by the existing pass,
  // but their own dependencies -- a custom building's textures, or its group files if the user
  // copied only the root in by hand -- may live anywhere in the chain.
  // The iteration itself can throw part way through -- a directory that disappears, or one the
  // process cannot descend into on a network share -- and losing the roots already found because
  // of a file nobody asked about would be the wrong trade.
  try
  {
    fs::recursive_directory_iterator iterator
      (project_path, fs::directory_options::skip_permission_denied, error);

    if (error)
    {
      return;
    }

    for (auto const& entry : iterator)
    {
      std::error_code entry_error;

      if (!entry.is_regular_file(entry_error) || entry_error)
      {
        continue;
      }

      std::string const relative
        (fs::relative(entry.path(), fs::path(project_path), entry_error).string());

      if (entry_error || relative.empty())
      {
        continue;
      }

      std::string const path (normalizeReferencePath(relative));

      if (path.empty() || !isTraversable(path))
      {
        continue;
      }

      // A WMO group file names nothing; opening it as a root would find no MOHD and report a parse
      // failure for a file that is perfectly good.
      if (isWmoGroupFileName(path))
      {
        continue;
      }

      QueuedReference root;
      root.path = path;
      root.referrer = "project folder";
      root.kind = assetKindFromPath(path);
      root.necessity = ReferenceNecessity::Required;
      root.traversable = true;
      root.is_root = true;

      queue.push_back(std::move(root));
      ++result.roots;
    }
  }
  catch (std::exception const& exception)
  {
    LogError << "PatchAssetPacker: stopped enumerating the project folder: "
             << exception.what() << std::endl;
  }
}

Noggit::PatchAssetPackerResult Noggit::PatchAssetPacker::run(ProgressFn const& progress)
{
  PatchAssetPackerResult result;

  if (!_client_data || !_target_archive)
  {
    return result;
  }

  std::vector<QueuedReference> roots;
  collectRoots(roots, result);

  std::deque<QueuedReference> queue (roots.begin(), roots.end());
  std::unordered_set<std::string> visited;

  // Every reference edge in discovery order, and one verdict per path.
  //
  // The counts are replayed after the walk rather than recorded during it, and they have to be: an
  // edge is discovered before its target's verdict is known -- the target is usually still in the
  // queue -- and AssetScanResult keeps the verdict of the FIRST call for an asset while counting
  // every later one as a disagreement. Recording an edge with a guessed status would make the
  // per-kind counts disagree with the failure list, which is worse than the wrong count it fixes.
  std::vector<ReferenceEdge> edges;
  std::unordered_map<std::string, PathVerdict> verdicts;

  // One copy of each distinct string, pointed at by every edge that uses it. std::unordered_set
  // never invalidates pointers to its elements, rehash included, so a pointer taken here stays
  // good for the whole walk.
  std::unordered_set<std::string> intern_pool;

  auto const intern
    ( [&intern_pool] (std::string const& text) -> std::string const*
      {
        return &*intern_pool.insert(text).first;
      }
    );

  // Paths some parent needs. An absent path that is optional everywhere is not reported; one that
  // is optional to a WMO skybox slot and required by an ADT is, which the old dequeue-time record
  // could not express because only the first discovering edge survived.
  std::unordered_set<std::string const*> required_somewhere;

  auto const record_edge
    ( [&edges, &required_somewhere, &intern]
      (std::string const& path, std::string const& referrer, AssetKind kind, ReferenceNecessity necessity)
      {
        ReferenceEdge edge;
        edge.path = intern(path);
        edge.referrer = intern(referrer);
        edge.kind = kind;
        edge.necessity = necessity;

        if (necessity == ReferenceNecessity::Required)
        {
          required_somewhere.insert(edge.path);
        }

        edges.push_back(edge);
      }
    );

  for (auto const& root : roots)
  {
    visited.insert(root.path);
    record_edge(root.path, root.referrer, root.kind, root.necessity);
  }

  std::vector<char> buffer;
  std::vector<AssetReference> children;

  while (!queue.empty())
  {
    QueuedReference const item (std::move(queue.front()));
    queue.pop_front();

    ++result.visited;

    if (progress && !progress("Collecting referenced assets", result.visited, result.visited + queue.size(), item.path))
    {
      // BREAK, not return: the edges recorded so far still have to be replayed, or a cancelled run
      // reports a scan result with no references in it at all.
      result.cancelled = true;
      break;
    }

    AssetOrigin const origin (originOf(item.path));

    if (origin == AssetOrigin::NotFound)
    {
      PathVerdict verdict;
      verdict.status = AssetStatus::Missing;
      verdict.absent = true;

      verdicts[item.path] = verdict;
      continue;
    }

    bool const wanted_for_pack
      ( !item.is_root
     && ( origin == AssetOrigin::CustomArchive
       || (origin == AssetOrigin::BaseClientArchive && _options.include_base_client_assets)
        )
      );

    bool const traverse (item.traversable && isTraversable(item.path));

    AssetStatus status = AssetStatus::Present;

    if (traverse || wanted_for_pack)
    {
      buffer.clear();

      try
      {
        // One read, not a probe followed by a read. ClientData::exists repeats the whole disk stat
        // plus per-archive walk, so exists() + ClientFile is two full lookups of the same path.
        BlizzardArchive::ClientFile file (item.path, _client_data);

        std::size_t const size = file.getSize();

        // A zero byte payload is not usable and is not distinguishable from absent once it is
        // inside an archive: ClientData::readFile skips empty files outright (ClientData.cpp:426).
        // Guarding explicitly matters because MPQArchive::writeFile's assert(buf_size) compiles to
        // nothing under NDEBUG, which RelWithDebInfo defines.
        if (file.isEof() || !size || !file.getBuffer())
        {
          status = AssetStatus::Unreadable;
        }
        else
        {
          buffer.assign(file.getBuffer(), file.getBuffer() + size);
        }
      }
      catch (...)
      {
        // Present according to the probe but unreadable in practice: a corrupt archive entry, or a
        // project file that vanished between the probe and the read.
        status = AssetStatus::Unreadable;
      }
    }

    {
      PathVerdict verdict;
      verdict.status = status;
      verdict.absent = false;

      verdicts[item.path] = verdict;
    }

    switch (origin)
    {
      case AssetOrigin::ProjectFolder:
        if (!item.is_root)
        {
          ++result.skipped_in_project;
        }
        break;
      case AssetOrigin::TargetArchive:
        if (!item.is_root)
        {
          ++result.skipped_already_in_patch;
        }
        break;
      case AssetOrigin::BaseClientArchive:
        if (!item.is_root && !_options.include_base_client_assets)
        {
          ++result.skipped_base_client;
        }
        break;
      default:
        break;
    }

    if (status != AssetStatus::Present)
    {
      continue;
    }

    if (wanted_for_pack)
    {
      try
      {
        fs::path const staged_path (nextStagingPath());
        std::ofstream out (staged_path, std::ios_base::binary | std::ios_base::out | std::ios_base::trunc);

        if (out.is_open())
        {
          out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
          out.close();

          StagedFile staged;
          // BACKSLASHES. MPQ resolves a file by hashing its name, so what is stored has to be
          // byte-for-byte what the client asks for -- and the client asks for
          // World\Maps\Expansion01\Expansion01_29_32.adt. This is the same conversion addFile
          // applies (MPQArchive.cpp:153-154), computed here so the name is one this code chose.
          staged.stored_name = archiveStoredName(item.path);
          staged.disk_path = staged_path;
          staged.size = buffer.size();

          _staged.push_back(std::move(staged));
        }
        else
        {
          ++result.write_failures;
          result.write_errors.push_back(item.path + ": could not create a staging file");
        }
      }
      catch (std::exception const& exception)
      {
        ++result.write_failures;
        result.write_errors.push_back(item.path + ": " + exception.what());
      }
    }

    if (!traverse)
    {
      continue;
    }

    children.clear();

    AssetParseResult const parsed (parseReferences(item.path, buffer, children));

    if (!parsed.complete())
    {
      // THE thing this whole struct exists for. Until it was recorded, a truncated ADT produced no
      // children, was counted as a successfully walked reference, and appeared in no list: the
      // patch shipped without every texture and model that file named while the report said
      // everything was fine. The file is still staged and still traversed as far as it went --
      // packing a half-readable file is strictly better than dropping it -- so nothing else in the
      // result will look wrong, which is exactly why this has to be said out loud.
      AssetParseFailure failure;
      failure.path = item.path;
      failure.referrer = item.referrer;
      failure.reason = parsed.reason;
      failure.partial = parsed.recognised() && !children.empty();
      failure.recovered = children.size();

      result.parse_failures.push_back(std::move(failure));
    }

    for (auto& child : children)
    {
      if (child.path.empty())
      {
        continue;
      }

      // Recorded for EVERY edge, queued only for a new target. Walking an already visited path
      // again would not terminate -- a WMO that names its own doodads is a cycle -- but counting
      // the edge costs nothing and is the difference between "(1 reference)" and the number that
      // decides which missing file is worth chasing first.
      record_edge(child.path, item.path, child.kind, child.necessity);

      if (!visited.insert(child.path).second)
      {
        continue;
      }

      QueuedReference queued;
      queued.path = std::move(child.path);
      queued.referrer = item.path;
      queued.kind = child.kind;
      queued.necessity = child.necessity;
      queued.traversable = child.traversable;

      queue.push_back(std::move(queued));
    }
  }

  // ---- reference counts -----------------------------------------------------------------------
  //
  // Replayed in discovery order, which is breadth-first from the roots and therefore identical
  // between two runs over an unchanged project. That matters for more than tidiness:
  // AssetScanResult keeps only the first DEFAULT_MAX_REFERRERS referrers per asset, so the order
  // decides which names the report prints.
  {
    std::unordered_set<std::string const*> optional_absent_counted;

    for (auto const& edge : edges)
    {
      auto const verdict (verdicts.find(*edge.path));

      if (verdict == verdicts.end())
      {
        // Queued and never reached, which happens only when the user cancelled.
        continue;
      }

      if (!verdict->second.absent)
      {
        result.scan.addReference(edge.kind, *edge.path, *edge.referrer, verdict->second.status);
        continue;
      }

      // Absent, and named only when some parent REQUIRES it. Optional references are the ones the
      // loaders themselves probe for -- external .anim files, a WMO skybox, a tileset's _s.blp
      // companion -- and absence is their normal case, so naming every one would bury the handful
      // that matter under thousands that were never expected to be there.
      //
      // Once a file is required by ANY parent, every edge to it counts, optional ones included: it
      // is on the missing list already and one more referrer is pure information. The old walk
      // could not express this at all, because only the first edge to reach a path survived and its
      // necessity decided the question for every other parent.
      if (required_somewhere.find(edge.path) != required_somewhere.end())
      {
        result.scan.addReference(edge.kind, *edge.path, *edge.referrer, AssetStatus::Missing);
        continue;
      }

      if (optional_absent_counted.insert(edge.path).second)
      {
        ++result.optional_absent;
      }
    }
  }

  // Deterministic order for the report; discovery order depends on how the filesystem enumerated
  // the project folder.
  std::sort( result.parse_failures.begin(), result.parse_failures.end()
           , [] (AssetParseFailure const& left, AssetParseFailure const& right)
             {
               return left.path < right.path;
             }
           );

  if (result.cancelled)
  {
    return result;
  }

  // ---- phase 2: write -------------------------------------------------------------------------

  // NOT `_staged.empty()` on its own. NoggitWindow hands saveLocalFilesToArchive
  // `compact_archive && !pack_dependencies` precisely so the rebuild is deferred to this pass, so a
  // run that stages nothing is the one case where nobody else will do it -- and returning here
  // silently dropped the compaction on every project whose references were all already present.
  if (_staged.empty() && !_options.compact)
  {
    return result;
  }

  bool opened = false;

  try
  {
    opened = _target_archive->openForWritting();
  }
  catch (std::exception const& exception)
  {
    result.write_errors.push_back(std::string("could not open the archive for writing: ") + exception.what());
  }

  if (!opened)
  {
    result.write_failures += _staged.size();

    if (_staged.empty())
    {
      // Nothing to write, so write_failures stays at zero and the summary would otherwise read as a
      // clean run that quietly did not compact.
      result.write_errors.push_back("could not open the archive to compact it");
    }

    return result;
  }

  std::size_t written = 0;

  for (auto const& staged : _staged)
  {
    ++written;

    if (progress && !progress("Adding referenced assets", written, _staged.size(), staged.stored_name))
    {
      result.cancelled = true;
      break;
    }

    try
    {
      // setFilepath, NOT the normalising FileKey constructor.
      //
      // Handing addFile a std::string would run it through ClientData::normalizeFilenameInternal,
      // whose .mdx rewrite is std::regex_replace(name, std::regex(".mdx"), ".m2") -- '.' matches
      // ANY character and every match is replaced, so a path such as creature/amdxthing/foo.m2
      // would be stored as creature/.m2thing/foo.m2 and hash to something no client will ever ask
      // for. That same function's ::tolower is also undefined for the bytes >= 0x80 these names
      // carry. setFilepath stores the string verbatim, and addFile's own '/' -> '\\' pass is then
      // a no-op on a name that is already backslash separated and lowercase.
      BlizzardArchive::Listfile::FileKey stored_key;
      stored_key.setFilepath(staged.stored_name);

      bool const success
        (_target_archive->addFile( stored_key
                                 , staged.disk_path.string()
                                 , _client_data->locale_mode()
                                 , 0
                                 , _options.compress
                                 ));

      if (success)
      {
        ++result.added;
        result.bytes_added += staged.size;
      }
      else
      {
        ++result.write_failures;
        result.write_errors.push_back(staged.stored_name + ": the archive refused the file");
      }
    }
    catch (std::exception const& exception)
    {
      ++result.write_failures;
      result.write_errors.push_back(staged.stored_name + ": " + exception.what());
    }
    catch (...)
    {
      ++result.write_failures;
      result.write_errors.push_back(staged.stored_name + ": unhandled exception");
    }
  }

  try
  {
    // Whenever it was asked for, not only when this pass added something. Gating on `added` meant
    // that ticking both Compact Archive and the dependency option on a project with nothing left to
    // pack skipped the compaction entirely: the dialog had already deferred it to here.
    if (_options.compact && !result.cancelled)
    {
      if (progress)
      {
        std::size_t const total = std::max<std::size_t>(_staged.size(), 1);

        progress("Compacting archive", total, total, std::string());
      }

      _target_archive->compactArchive();
    }
  }
  catch (std::exception const& exception)
  {
    result.write_errors.push_back(std::string("compacting failed: ") + exception.what());
  }

  // Unconditional: leaving the archive open for writing would leave every later read going through
  // a writable handle, and closeToReadOnly is the only thing that puts the read-only stream flag
  // back.
  try
  {
    _target_archive->closeToReadOnly();
  }
  catch (std::exception const& exception)
  {
    result.write_errors.push_back(std::string("could not reopen the archive read-only: ") + exception.what());
  }

  removeStagingDirectory();

  return result;
}
