// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/AssetScan.hpp>

#include <algorithm>
#include <array>
#include <sstream>
#include <utility>

using namespace Noggit;

namespace
{
  // Backslash, matching what an ADT texture list, an MMDX block and a DBC string table all
  // actually contain, and what MPQArchive asks StormLib for after normalizeFilenameWoW
  // (MPQArchive.cpp:39). A report that echoes the map's own spelling is easier to grep the data
  // for than one that has silently switched to '/'.
  constexpr char SEPARATOR = '\\';

  // Explicit set rather than std::isspace, which takes an int, is locale-sensitive and has
  // undefined behaviour for negative values -- i.e. for every byte >= 0x80 once a signed char is
  // promoted. Asset name tables do carry such bytes.
  bool isTrimmableSpace(char c)
  {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
  }

  bool isSeparator(char c)
  {
    return c == '\\' || c == '/';
  }

  char toLowerAscii(char c)
  {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  }

  std::string_view trimmed(std::string_view text)
  {
    std::size_t begin = 0;
    std::size_t end = text.size();

    while (begin < end && isTrimmableSpace(text[begin]))
    {
      ++begin;
    }

    while (end > begin && isTrimmableSpace(text[end - 1]))
    {
      --end;
    }

    return text.substr(begin, end - begin);
  }

  // Lowercased extension without the dot, or empty when there is none. Searched only after the
  // last separator, so "world/tex.set/foo" is correctly reported as having no extension rather
  // than an extension of "set/foo".
  std::string extensionOf(std::string_view path)
  {
    std::size_t const last_separator = path.find_last_of("\\/");
    std::size_t const search_from = (last_separator == std::string_view::npos) ? 0 : last_separator + 1;
    std::size_t const dot = path.find_last_of('.');

    if (dot == std::string_view::npos || dot < search_from || dot + 1 >= path.size())
    {
      return {};
    }

    std::string extension (path.substr(dot + 1));
    std::transform(extension.begin(), extension.end(), extension.begin(), toLowerAscii);

    return extension;
  }

  void appendReferrer(std::vector<std::string>& referrers, std::string_view referrer, std::size_t max_referrers)
  {
    if (referrer.empty() || referrers.size() >= max_referrers)
    {
      return;
    }

    // Linear, but bounded by max_referrers (8 by default), so this is a handful of comparisons
    // and not worth a set per asset.
    bool const known = std::any_of( referrers.begin()
                                  , referrers.end()
                                  , [referrer] (std::string const& known_referrer)
                                    {
                                      return std::string_view(known_referrer) == referrer;
                                    }
                                  );

    if (!known)
    {
      referrers.emplace_back(referrer);
    }
  }

  // Report/sort order. Declared once so failures() and toReport() cannot disagree about it.
  constexpr std::array<AssetKind, 4> KIND_ORDER
    {AssetKind::Model, AssetKind::WorldModel, AssetKind::Texture, AssetKind::Other};

  int kindRank(AssetKind kind)
  {
    for (std::size_t i = 0; i < KIND_ORDER.size(); ++i)
    {
      if (KIND_ORDER[i] == kind)
      {
        return static_cast<int>(i);
      }
    }

    return static_cast<int>(KIND_ORDER.size());
  }
}

std::string_view Noggit::assetKindName(AssetKind kind)
{
  switch (kind)
  {
    case AssetKind::Model:      return "model";
    case AssetKind::WorldModel: return "world model";
    case AssetKind::Texture:    return "texture";
    case AssetKind::Other:      return "other";
  }

  return "other";
}

AssetKind Noggit::assetKindFromPath(std::string_view path)
{
  std::string const extension (extensionOf(trimmed(path)));

  if (extension == "m2" || extension == "mdx" || extension == "mdl")
  {
    return AssetKind::Model;
  }

  if (extension == "wmo")
  {
    return AssetKind::WorldModel;
  }

  if (extension == "blp")
  {
    return AssetKind::Texture;
  }

  return AssetKind::Other;
}

std::string Noggit::normalizeAssetKey(std::string_view path)
{
  std::string_view const source = trimmed(path);

  std::string key;
  key.reserve(source.size());

  bool at_separator = true; // leading separators are dropped, so start as if one was just written

  for (char const c : source)
  {
    if (isSeparator(c))
    {
      if (!at_separator)
      {
        key.push_back(SEPARATOR);
        at_separator = true;
      }

      continue;
    }

    key.push_back(toLowerAscii(c));
    at_separator = false;
  }

  // A trailing separator means the string names a directory, not a file. Dropping it rather than
  // rejecting the whole path keeps "world\foo.m2\" usable, and leaves the empty-string return for
  // input that carried no name at all.
  while (!key.empty() && key.back() == SEPARATOR)
  {
    key.pop_back();
  }

  return key;
}

bool AssetScanResult::addReference( AssetKind kind
                                  , std::string_view path
                                  , std::string_view referrer
                                  , AssetStatus status
                                  , std::size_t max_referrers
                                  )
{
  std::string const key = normalizeAssetKey(path);

  if (key.empty())
  {
    ++_rejected;
    return false;
  }

  ++_total_references;

  auto const inserted = _entries.try_emplace(key);
  Entry& entry = inserted.first->second;

  if (inserted.second)
  {
    entry.display_path = std::string(path);
    entry.kind = kind;
    entry.status = status;
  }
  else if (entry.status != status)
  {
    ++_disagreements;
  }

  ++entry.reference_count;
  appendReferrer(entry.referrers, referrer, max_referrers);

  return true;
}

std::size_t AssetScanResult::totalReferences() const
{
  return _total_references;
}

std::size_t AssetScanResult::distinctReferenced() const
{
  return _entries.size();
}

std::size_t AssetScanResult::distinctMissing() const
{
  return static_cast<std::size_t>
    (std::count_if( _entries.begin()
                  , _entries.end()
                  , [] (auto const& entry) { return entry.second.status == AssetStatus::Missing; }
                  )
    );
}

std::size_t AssetScanResult::distinctUnreadable() const
{
  return static_cast<std::size_t>
    (std::count_if( _entries.begin()
                  , _entries.end()
                  , [] (auto const& entry) { return entry.second.status == AssetStatus::Unreadable; }
                  )
    );
}

std::size_t AssetScanResult::rejectedCount() const
{
  return _rejected;
}

std::size_t AssetScanResult::disagreementCount() const
{
  return _disagreements;
}

AssetKindCounts AssetScanResult::counts(AssetKind kind) const
{
  AssetKindCounts result;

  for (auto const& entry : _entries)
  {
    if (entry.second.kind != kind)
    {
      continue;
    }

    ++result.referenced;

    if (entry.second.status == AssetStatus::Missing)
    {
      ++result.missing;
    }
    else if (entry.second.status == AssetStatus::Unreadable)
    {
      ++result.unreadable;
    }
  }

  return result;
}

bool AssetScanResult::empty() const
{
  return _entries.empty();
}

bool AssetScanResult::hasFailures() const
{
  return std::any_of( _entries.begin()
                    , _entries.end()
                    , [] (auto const& entry) { return entry.second.status != AssetStatus::Present; }
                    );
}

std::vector<MissingAsset> AssetScanResult::failures() const
{
  std::vector<MissingAsset> result;

  for (auto const& entry : _entries)
  {
    if (entry.second.status == AssetStatus::Present)
    {
      continue;
    }

    MissingAsset asset;
    asset.key = entry.first;
    asset.display_path = entry.second.display_path;
    asset.kind = entry.second.kind;
    asset.status = entry.second.status;
    asset.reference_count = entry.second.reference_count;
    asset.referrers = entry.second.referrers;

    result.push_back(std::move(asset));
  }

  // Total order, and the last tiebreak is the key, which is unique. Without that last term the
  // ordering would depend on the unordered_map's bucket walk and two scans of the same map could
  // produce different reports.
  std::sort( result.begin()
           , result.end()
           , [] (MissingAsset const& lhs, MissingAsset const& rhs)
             {
               int const lhs_rank = kindRank(lhs.kind);
               int const rhs_rank = kindRank(rhs.kind);

               if (lhs_rank != rhs_rank)
               {
                 return lhs_rank < rhs_rank;
               }

               if (lhs.reference_count != rhs.reference_count)
               {
                 return lhs.reference_count > rhs.reference_count;
               }

               return lhs.key < rhs.key;
             }
           );

  return result;
}

std::vector<MissingAsset> AssetScanResult::failures(AssetKind kind) const
{
  std::vector<MissingAsset> result = failures();

  result.erase( std::remove_if( result.begin()
                              , result.end()
                              , [kind] (MissingAsset const& asset) { return asset.kind != kind; }
                              )
              , result.end()
              );

  return result;
}

bool AssetScanResult::wasReferenced(std::string_view path) const
{
  return _entries.find(normalizeAssetKey(path)) != _entries.end();
}

AssetStatus AssetScanResult::statusOf(std::string_view path, AssetStatus fallback) const
{
  auto const entry = _entries.find(normalizeAssetKey(path));

  return entry == _entries.end() ? fallback : entry->second.status;
}

std::size_t AssetScanResult::referenceCount(std::string_view path) const
{
  auto const entry = _entries.find(normalizeAssetKey(path));

  return entry == _entries.end() ? 0 : entry->second.reference_count;
}

void AssetScanResult::mergeEntry(std::string const& key, Entry const& entry, std::size_t max_referrers)
{
  auto const inserted = _entries.try_emplace(key);
  Entry& target = inserted.first->second;

  if (inserted.second)
  {
    target.display_path = entry.display_path;
    target.kind = entry.kind;
    target.status = entry.status;
  }
  else if (target.status != entry.status)
  {
    ++_disagreements;
  }

  target.reference_count += entry.reference_count;

  for (std::string const& referrer : entry.referrers)
  {
    appendReferrer(target.referrers, referrer, max_referrers);
  }
}

void AssetScanResult::merge(AssetScanResult const& other, std::size_t max_referrers)
{
  for (auto const& entry : other._entries)
  {
    mergeEntry(entry.first, entry.second, max_referrers);
  }

  _total_references += other._total_references;
  _rejected += other._rejected;
  _disagreements += other._disagreements;
}

std::string AssetScanResult::summary() const
{
  std::ostringstream out;

  std::size_t const missing = distinctMissing();
  std::size_t const unreadable = distinctUnreadable();

  if (missing == 0 && unreadable == 0)
  {
    out << "Asset scan: no missing assets, " << distinctReferenced() << " referenced ("
        << _total_references << " references)";
  }
  else
  {
    out << "Asset scan: " << missing << " missing, " << unreadable << " unreadable of "
        << distinctReferenced() << " referenced (" << _total_references << " references)";
  }

  return out.str();
}

std::string AssetScanResult::toReport() const
{
  std::ostringstream out;

  out << "Missing asset scan\n";
  out << "==================\n";
  out << summary() << "\n";

  if (_rejected != 0)
  {
    out << _rejected << " reference(s) rejected as unusable paths\n";
  }

  // Surfaced rather than swallowed: a non-zero count means an asset's verdict changed mid-scan,
  // so the numbers above are a snapshot of a chain that moved and a re-run may differ.
  if (_disagreements != 0)
  {
    out << _disagreements << " reference(s) disagreed with an earlier verdict for the same asset\n";
  }

  out << "\n";

  for (AssetKind const kind : KIND_ORDER)
  {
    AssetKindCounts const kind_counts = counts(kind);

    if (kind_counts.referenced == 0)
    {
      continue;
    }

    out << assetKindName(kind) << ": " << kind_counts.referenced << " referenced, "
        << kind_counts.missing << " missing, " << kind_counts.unreadable << " unreadable\n";
  }

  std::vector<MissingAsset> const failed = failures();

  if (failed.empty())
  {
    out << "\nNothing missing.\n";
    return out.str();
  }

  // Not grouped into a "missing" and an "unreadable" section: failures() sorts by kind first,
  // because kind is what the reader acts on (a missing .blp and a missing .m2 are different jobs),
  // and a per-line status prefix loses nothing that a section header would have carried.
  for (MissingAsset const& asset : failed)
  {
    out << "\n" << ((asset.status == AssetStatus::Missing) ? "[missing]    " : "[unreadable] ")
        << "(" << assetKindName(asset.kind) << ") " << asset.display_path
        << "  x" << asset.reference_count;

    if (!asset.referrers.empty())
    {
      out << "\n             referenced by: ";

      for (std::size_t i = 0; i < asset.referrers.size(); ++i)
      {
        out << (i == 0 ? "" : ", ") << asset.referrers[i];
      }

      // The cap is per asset and the count above is exact, so a truncated list is obvious from
      // the arithmetic rather than silently short.
      if (asset.referrers.size() < asset.reference_count)
      {
        out << ", ...";
      }
    }

    out << "\n";
  }

  return out.str();
}

AssetScanner::AssetScanner(ExistsFn exists_fn, AssetScanOptions options)
  : _exists_fn (std::move(exists_fn))
  , _options (options)
{
}

void AssetScanner::addReference( AssetKind kind
                               , std::string_view path
                               , std::string_view referrer
                               , bool already_failed_to_load
                               )
{
  std::string const key = normalizeAssetKey(path);

  if (key.empty())
  {
    // Still routed through the result, so rejectedCount() stays the single place unusable paths
    // are counted and a caller cannot see a reference vanish without a number moving.
    _result.addReference(kind, path, referrer, AssetStatus::Missing, _options.max_referrers_per_asset);
    return;
  }

  auto const cached = _probe_cache.find(key);
  bool present = false;

  if (cached != _probe_cache.end())
  {
    present = cached->second;
  }
  else
  {
    // The key, not the original spelling: see makeClientDataProbe on why a leading separator
    // breaks the on-disk half of the probe.
    present = _exists_fn ? _exists_fn(key) : false;
    ++_probe_count;
    _probe_cache.emplace(key, present);
  }

  AssetStatus status = AssetStatus::Present;

  if (!present)
  {
    status = AssetStatus::Missing;
  }
  else if (already_failed_to_load)
  {
    status = AssetStatus::Unreadable;
  }

  _result.addReference(kind, path, referrer, status, _options.max_referrers_per_asset);
}

void AssetScanner::addReference(std::string_view path, std::string_view referrer, bool already_failed_to_load)
{
  addReference(assetKindFromPath(path), path, referrer, already_failed_to_load);
}

AssetScanResult const& AssetScanner::result() const
{
  return _result;
}

AssetScanResult AssetScanner::takeResult()
{
  AssetScanResult taken = std::move(_result);

  // Moved-from, so the probe cache would no longer describe what the result holds. Clearing both
  // makes a scanner reused after takeResult start clean instead of reporting counts that belong
  // to a result someone else now owns.
  _result = AssetScanResult();
  _probe_cache.clear();
  _probe_count = 0;

  return taken;
}

AssetScanOptions const& AssetScanner::options() const
{
  return _options;
}

std::size_t AssetScanner::probeCount() const
{
  return _probe_count;
}
