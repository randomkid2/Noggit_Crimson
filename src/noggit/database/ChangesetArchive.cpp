// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/database/ChangesetArchive.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ios>
#include <istream>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

using namespace Noggit::Database;

namespace
{
  constexpr char const* EXTENSION = ".sql";

  // Substituted when a label sanitises away to nothing. Naming a changeset something bland
  // cannot lose it; refusing to store one can.
  constexpr char const* DEFAULT_LABEL = "changeset";

  // Layout of the fixed-width part of a name: YYYYMMDD-HHMMSS-NNN-<label>
  //
  // Spelled out as offsets because both the writer and the reader index into it, and the one
  // way this scheme can fail is for those two to disagree by a character.
  constexpr std::size_t DATE_AT = 0;
  constexpr std::size_t DATE_LENGTH = 8;
  constexpr std::size_t TIME_AT = 9;
  constexpr std::size_t TIME_LENGTH = 6;
  constexpr std::size_t SEQUENCE_AT = 16;
  constexpr std::size_t SEQUENCE_LENGTH = 3;
  constexpr std::size_t LABEL_AT = 20;

  constexpr unsigned int MAX_SEQUENCE = 999;

  constexpr std::uint64_t SECONDS_PER_MINUTE = 60;
  constexpr std::uint64_t SECONDS_PER_HOUR = 3600;
  constexpr std::uint64_t SECONDS_PER_DAY = 86400;

  // 9999-12-31T23:59:59Z. One second later the year needs a fifth digit and the fixed-width text
  // ordering the whole naming scheme rests on quietly stops holding, because "10000..." sorts
  // BEFORE "9999...". Rather than widen a field for a date no changeset will ever carry,
  // anything past this is treated as what it always is in practice: milliseconds passed where
  // seconds were expected.
  constexpr std::uint64_t MAX_TIMESTAMP = 253402300799ull;

  // Zero-padded fixed-width decimal.
  //
  // Imbued with the classic locale even though no decimal point is involved: num_put consults
  // the numpunct grouping facet for integers too, so under a global locale that groups
  // thousands an unimbued stream renders 20260801 as "20,260,801" and every name in the archive
  // stops matching the scheme -- including, worse, only the names written after some other part
  // of the program installed that locale.
  std::string padded(std::uint64_t value, std::size_t width)
  {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setfill('0') << std::setw(static_cast<int>(width)) << value;

    return out.str();
  }

  // "YYYYMMDD-HHMMSS", UTC.
  //
  // The calendar conversion goes through the C++20 chrono types rather than gmtime: gmtime_r and
  // gmtime_s are spelled differently per platform, plain gmtime hands back shared mutable state,
  // and strftime formats through the C locale. year_month_day has none of those properties and
  // is exact for every value this function accepts.
  //
  // The time of day is plain integer arithmetic. `timestamp` is unsigned, so the division needs
  // no flooring and cannot be caught out by a pre-epoch value the way a signed time_t can.
  std::string stampFor(std::uint64_t timestamp)
  {
    std::chrono::sys_days const day
      (std::chrono::days(static_cast<std::chrono::days::rep>(timestamp / SECONDS_PER_DAY)));

    std::chrono::year_month_day const date (day);

    std::uint64_t const time_of_day (timestamp % SECONDS_PER_DAY);

    return padded(static_cast<std::uint64_t>(static_cast<int>(date.year())), 4)
         + padded(static_cast<unsigned int>(date.month()), 2)
         + padded(static_cast<unsigned int>(date.day()), 2)
         + "-"
         + padded(time_of_day / SECONDS_PER_HOUR, 2)
         + padded((time_of_day / SECONDS_PER_MINUTE) % SECONDS_PER_MINUTE, 2)
         + padded(time_of_day % SECONDS_PER_MINUTE, 2);
  }

  // Stands in for any code unit of a file name that is not ASCII. See asciiView.
  constexpr char NON_ASCII_PLACEHOLDER = '?';

  // A file name reduced to the ASCII that the naming scheme and the safety checks are made of.
  //
  // Deliberately not path::string(). That converts through the platform's narrow encoding --
  // the active code page on Windows -- which either loses the name or refuses to render it at
  // all: on MSVC 19.4x under code page 1252 it throws for a name the page cannot express. Doing
  // it here instead means a foreign name costs a placeholder character in a listing rather than
  // an exception out of every entry point. See ArchiveEntry::file_name.
  //
  // Every native code unit above 0x7F becomes one placeholder byte instead, whatever the native
  // encoding happens to be, and that substitution is exact for every character anything here
  // looks for. In UTF-8 no ASCII character is ever encoded by a byte above 0x7F, and in UTF-16
  // no ASCII character is ever encoded by a code unit above 0x7F -- so a separator, a colon, a
  // dot or a control character in the view is really in the name, and one in the name is always
  // in the view. Nothing this function produces is ever used to open, delete or address a file.
  std::string asciiView(std::filesystem::path const& name)
  {
    using value_type = std::filesystem::path::value_type;

    std::filesystem::path::string_type const& native (name.native());

    std::string view;
    view.reserve(native.size());

    for (value_type const unit : native)
    {
      // Widened as unsigned: wchar_t is unsigned on Windows, but plain char -- the POSIX
      // value_type -- is signed on every compiler that matters, so a UTF-8 continuation byte
      // would otherwise compare as negative and slip under the 0x80 bound as if it were ASCII.
      auto const code (static_cast<std::make_unsigned_t<value_type>>(unit));

      view.push_back(code < 0x80 ? static_cast<char>(code) : NON_ASCII_PLACEHOLDER);
    }

    return view;
  }

  // A narrow rendering of a path for a MESSAGE, never a handle to anything.
  //
  // path::string() is the obvious choice and the wrong one twice over: it is lossy exactly
  // where a foreign name needs spelling out, and on MSVC it throws for such a name -- from
  // inside the construction of an error message, which would replace a diagnosable failure with
  // an unrelated exception at the moment the user most needs to be told what went wrong.
  // u8string() spells every native name exactly; the two fallbacks are there so that this
  // function can never itself be the reason a failure goes unreported.
  std::string describe(std::filesystem::path const& path)
  {
    try
    {
      std::u8string const utf8 (path.u8string());

      return std::string(reinterpret_cast<char const*>(utf8.data()), utf8.size());
    }
    catch (std::exception const&)
    {
    }

    try
    {
      return path.string();
    }
    catch (std::exception const&)
    {
    }

    return "<a name this platform cannot render as text>";
  }

  bool isDigits(std::string const& s, std::size_t at, std::size_t count)
  {
    if (s.size() < at + count)
    {
      return false;
    }

    for (std::size_t i (at); i < at + count; ++i)
    {
      if (s[i] < '0' || s[i] > '9')
      {
        return false;
      }
    }

    return true;
  }

  // Digits only, already checked by isDigits. Accumulated by hand rather than through stoul so
  // there is no locale, no errno and no exception in the middle of a directory scan.
  std::uint64_t number(std::string const& s, std::size_t at, std::size_t count)
  {
    std::uint64_t value (0);

    for (std::size_t i (at); i < at + count; ++i)
    {
      value = value * 10 + static_cast<std::uint64_t>(s[i] - '0');
    }

    return value;
  }

  // Inverse of stampFor. False when the digits are not a real UTC instant.
  //
  // That check is what separates a name this archive wrote from one that merely looks like it:
  // 20260231 is eight digits and is not a date, and year_month_day::ok() is the cheapest honest
  // way to know the difference.
  bool timestampFromStamp(std::string const& stem, std::uint64_t& timestamp)
  {
    std::chrono::year_month_day const date
      ( std::chrono::year(static_cast<int>(number(stem, DATE_AT, 4)))
      / std::chrono::month(static_cast<unsigned int>(number(stem, DATE_AT + 4, 2)))
      / std::chrono::day(static_cast<unsigned int>(number(stem, DATE_AT + 6, 2)))
      );

    if (!date.ok())
    {
      return false;
    }

    std::uint64_t const hours (number(stem, TIME_AT, 2));
    std::uint64_t const minutes (number(stem, TIME_AT + 2, 2));
    std::uint64_t const seconds (number(stem, TIME_AT + 4, 2));

    // A leap second, 23:59:60, is rejected along with plain nonsense. Unix time does not count
    // them, so store() cannot have produced one.
    if (hours > 23 || minutes > 59 || seconds > 59)
    {
      return false;
    }

    std::int64_t const days
      (static_cast<std::int64_t>(std::chrono::sys_days(date).time_since_epoch().count()));

    // Pre-epoch: store() takes an unsigned timestamp, so it did not write this.
    if (days < 0)
    {
      return false;
    }

    timestamp = static_cast<std::uint64_t>(days) * SECONDS_PER_DAY
              + hours * SECONDS_PER_HOUR
              + minutes * SECONDS_PER_MINUTE
              + seconds;

    return true;
  }

  std::string lowered(std::string s)
  {
    for (char& c : s)
    {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    return s;
  }

  struct ParsedName
  {
    std::uint64_t timestamp = 0;
    unsigned int sequence = 0;
    std::string label;
  };

  // True when `file_name` is one this archive wrote, in which case `parsed` is filled in.
  //
  // The extension is found with rfind rather than std::filesystem::path::extension so that a
  // name coming off the filesystem is examined as the bytes it is. A label may legitimately
  // contain a dot ("v1.2"), so it is the LAST dot that separates the extension.
  bool parseName(std::string const& file_name, ParsedName& parsed)
  {
    std::size_t const dot (file_name.rfind('.'));

    if (dot == std::string::npos || lowered(file_name.substr(dot)) != EXTENSION)
    {
      return false;
    }

    std::string const stem (file_name.substr(0, dot));

    // Strictly greater: a name with no label at all is not one store() can produce, because
    // sanitiseLabel never returns an empty string.
    if (stem.size() <= LABEL_AT)
    {
      return false;
    }

    if (!isDigits(stem, DATE_AT, DATE_LENGTH) || stem[DATE_AT + DATE_LENGTH] != '-')
    {
      return false;
    }

    if (!isDigits(stem, TIME_AT, TIME_LENGTH) || stem[TIME_AT + TIME_LENGTH] != '-')
    {
      return false;
    }

    if (!isDigits(stem, SEQUENCE_AT, SEQUENCE_LENGTH)
        || stem[SEQUENCE_AT + SEQUENCE_LENGTH] != '-')
    {
      return false;
    }

    if (!timestampFromStamp(stem, parsed.timestamp))
    {
      return false;
    }

    parsed.sequence = static_cast<unsigned int>(number(stem, SEQUENCE_AT, SEQUENCE_LENGTH));
    parsed.label = stem.substr(LABEL_AT);

    return true;
  }

  // Every name this class resolves against the root was ultimately built out of caller-supplied
  // text, so nothing but a single ordinary file name component is accepted.
  //
  // The test is deliberately lexical. A name holding no path separator, no colon and no ".."
  // cannot address anything but a direct child of the root, so resolving it against the
  // filesystem first -- canonicalising and comparing prefixes -- could only add failure modes of
  // its own, not safety. Symbolic links are handled where they actually matter, by the directory
  // scan, which never reports one.
  void requireBareFileName(std::filesystem::path const& file_name)
  {
    // Every lexical test below runs over the ASCII view rather than over the native name.
    // Everything being looked for -- a separator, a colon, a control character, a trailing dot
    // or space, a reserved device name -- is ASCII, and asciiView neither invents one nor hides
    // one, so the view answers these questions exactly while a narrow conversion would not.
    std::string const name (asciiView(file_name));

    auto const refuse = [&file_name] (char const* why) -> void
    {
      throw ArchiveError
        ("Refusing the changeset backup name '" + describe(file_name) + "': " + why
         + ". Only a bare file name, as returned by list(), addresses an archived changeset.");
    };

    if (name.empty())
    {
      refuse("it is empty");
    }

    if (name == "." || name == "..")
    {
      refuse("it names a directory, not a file");
    }

    for (char const c : name)
    {
      unsigned char const byte (static_cast<unsigned char>(c));

      if (byte == '/' || byte == '\\')
      {
        refuse("it holds a path separator");
      }

      if (byte == ':')
      {
        refuse("it holds a drive or alternate-data-stream separator");
      }

      if (byte < 0x20)
      {
        refuse("it holds a control character");
      }
    }

    // Win32 normalises a final component before it reaches the filesystem, so a purely lexical
    // byte test is not sufficient on its own.
    //
    // Trailing spaces and dots are stripped, which turns ".. " -- not equal to ".." and holding
    // no separator, so it passes every check above -- into the archive root's PARENT directory.
    if (name.back() == ' ' || name.back() == '.')
    {
      refuse("it ends in a space or a dot, which Windows strips before resolving the path");
    }

    // A reserved DOS device name resolves to the device, not to a child of the root: load("NUL")
    // would open the null device, report a size of 0 with no error, and hand back an empty string
    // as a SUCCESSFUL recovery of a backup that does not exist. Silently returning nothing is the
    // worst outcome this class has, given its whole purpose is that a lost changeset is
    // recoverable.
    {
      std::string stem (name.substr(0, name.find('.')));

      for (char& c : stem)
      {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }

      static char const* const RESERVED_DEVICE_NAMES[]
        { "con", "prn", "aux", "nul"
        , "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9"
        , "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"
        };

      for (char const* reserved : RESERVED_DEVICE_NAMES)
      {
        if (stem == reserved)
        {
          refuse("it names a reserved DOS device, which resolves to hardware rather than a file");
        }
      }
    }

    // Belt and braces, in case a platform spells a separator in some way the loop above does not
    // know about. Asked of the path directly rather than of a path rebuilt from text: the
    // caller already holds the native name, and re-encoding it to ask a question about it is
    // how the narrow round trip got in here in the first place.
    bool const single_component
      (  !file_name.has_root_path()
      && !file_name.has_parent_path()
      && file_name.filename() == file_name
      );

    if (!single_component)
    {
      refuse("it is not a single path component");
    }
  }
}

ChangesetArchive::ChangesetArchive(std::filesystem::path root)
  : _root (std::move(root))
{
  if (_root.empty())
  {
    throw ArchiveError
      ("The changeset archive needs an explicit root directory. An empty path resolves against"
       " the process working directory, which would scatter backups wherever Noggit happened to"
       " be started from -- unfindable at exactly the moment they are wanted.");
  }
}

std::filesystem::path ChangesetArchive::store
  ( std::string const& sql
  , std::string const& label
  , std::uint64_t timestamp
  )
{
  if (timestamp > MAX_TIMESTAMP)
  {
    throw ArchiveError
      ("Refusing to archive a changeset stamped " + std::to_string(timestamp)
       + ": the archive names files in UTC and reaches only 9999-12-31T23:59:59Z ("
       + std::to_string(MAX_TIMESTAMP) + " seconds). A value this large is almost always"
       " milliseconds passed where seconds were expected, and storing it would put the file at"
       " the wrong end of every listing.");
  }

  ensureRoot();

  std::string const file_name (nextFileName(timestamp, sanitiseLabel(label)));
  std::filesystem::path const path (_root / file_name);

  // Binary, so the payload on disk is the bytes the caller handed over and nothing else. Text
  // mode on Windows rewrites every \n as \r\n on the way out, which makes load() disagree with
  // store() by one byte per line and destroys the only property that matters here: that what
  // comes back is what went in. A changeset also legitimately contains UTF-8, which no mode
  // should be interpreting.
  // Exclusive create, not truncate.
  //
  // nextFileName picks an unused name with exists(), which is a check, not a claim: between that
  // call and this open another process sharing the archive can create the same file. Opening with
  // std::ios::trunc would then silently destroy its changeset -- losing exactly the file this
  // class exists to keep. There is no exclusive-create flag on std::ofstream, so the file is
  // claimed with fopen's C11 "x" mode, which fails rather than truncates when the name already
  // exists, and the stream is attached to the descriptor that claim produced.
  std::FILE* claimed (nullptr);

#ifdef _MSC_VER
  if (::_wfopen_s(&claimed, path.wstring().c_str(), L"wbx") != 0)
  {
    claimed = nullptr;
  }
#else
  claimed = std::fopen(path.string().c_str(), "wbx");
#endif

  if (!claimed)
  {
    throw ArchiveError
      ("Cannot create the changeset backup '" + path.string() + "'. Either the name was claimed"
       " by another process between choosing it and creating it, or the directory is not"
       " writable. The changeset has NOT been archived; retrying will choose a new name.");
  }

  // Binary, so the payload on disk is the bytes the caller handed over and nothing else. Text
  // mode on Windows rewrites every \n as \r\n on the way out, which makes load() disagree with
  // store() by one byte per line and destroys the only property that matters here: that what
  // comes back is what went in. A changeset also legitimately contains UTF-8, which no mode
  // should be interpreting.
  bool write_failed (false);

  if (!sql.empty())
  {
    write_failed = std::fwrite(sql.data(), 1, sql.size(), claimed) != sql.size();
  }

  // Closed explicitly rather than left to a destructor: a failure at close is the normal way a
  // full volume reports itself, and a destructor swallows it.
  bool const close_failed (std::fclose(claimed) != 0);

  if (write_failed || close_failed)
  {
    throw ArchiveError
      ("Failed to write the changeset backup '" + path.string() + "'. The partial file has been"
       " left in place deliberately -- half a changeset a human can read beats none.");
  }

  // A successful write and close is not quite enough on its own. A short write can surface only
  // at close, and a stray text-mode translation would change the length without reporting an
  // error anywhere. One stat turns both into a loud failure instead of a backup that is quietly
  // not a copy of what was emitted.
  std::error_code ec;
  std::uintmax_t const written (std::filesystem::file_size(path, ec));

  if (ec || written != static_cast<std::uintmax_t>(sql.size()))
  {
    throw ArchiveError
      ("The changeset backup '" + path.string() + "' holds " + std::to_string(written)
       + " bytes but the changeset is " + std::to_string(sql.size())
       + ". The file has been left in place for inspection; treat it as incomplete.");
  }

  return path;
}

std::vector<ArchiveEntry> ChangesetArchive::list() const
{
  std::vector<StoredFile> const found (collect());

  std::vector<ArchiveEntry> entries;
  entries.reserve(found.size());

  for (StoredFile const& stored : found)
  {
    entries.push_back(stored.entry);
  }

  // Sorted on the file NAME descending, not on the decoded timestamp. The two orders are the
  // same by construction, because everything before the label is fixed width -- and sorting on
  // the name is what keeps that true: if the format ever loses the property, this ordering is
  // where it shows up, rather than list() quietly disagreeing with every directory listing and
  // with the order a recovering user sees in their file manager.
  std::sort
    ( entries.begin(), entries.end()
    , [] (ArchiveEntry const& a, ArchiveEntry const& b)
      {
        // On the native strings, not through path::operator>, which orders by path ELEMENT and
        // would stop being a plain text comparison the moment a name grew a separator. These
        // are single components by construction, so the two agree today; saying which one is
        // meant is what keeps the ordering promise checkable.
        return a.file_name.native() > b.file_name.native();
      }
    );

  return entries;
}

std::string ChangesetArchive::load(std::filesystem::path const& file_name) const
{
  requireBareFileName(file_name);
  requireRoot();

  std::filesystem::path const path (_root / file_name);

  // Built once and handed to readExactly, which knows how to read a file but not how to name
  // one: the archive root is this object's business and does not belong in a free function's
  // parameter list.
  std::string const description ("'" + describe(file_name) + "' in '" + _root.string() + "'");

  std::error_code ec;
  std::uintmax_t const size (std::filesystem::file_size(path, ec));

  if (ec)
  {
    throw ArchiveError
      ("No changeset backup named " + description + ": " + ec.message());
  }

  std::ifstream file (path, std::ios::in | std::ios::binary);

  if (!file)
  {
    throw ArchiveError("Cannot open the changeset backup " + description + ".");
  }

  return readExactly(file, size, description);
}

std::size_t ChangesetArchive::prune(std::size_t keep_newest)
{
  std::vector<ArchiveEntry> const entries (list());

  std::size_t removed (0);

  for (std::size_t index (keep_newest); index < entries.size(); ++index)
  {
    std::filesystem::path const& file_name (entries[index].file_name);

    // Every name here came out of a non-recursive scan of the root and parsed as one this class
    // writes, so it cannot address anything else. Checked again all the same: deletion is the
    // one operation whose mistakes cannot be undone, and the check costs nothing.
    requireBareFileName(file_name);

    std::error_code ec;

    // remove() answers false without an error when the file is already gone -- another process
    // pruning the same archive, or a user deleting by hand between the scan and here. That is
    // not a failure and is not counted as a removal either.
    if (std::filesystem::remove(_root / file_name, ec))
    {
      ++removed;
    }

    if (ec)
    {
      throw ArchiveError
        ("Failed to prune the changeset backup '" + describe(file_name) + "' from '"
         + _root.string() + "': " + ec.message() + ". " + std::to_string(removed) + " older"
         " backup(s) had already been removed, so the archive is partly pruned.");
    }
  }

  return removed;
}

std::string ChangesetArchive::sanitiseLabel(std::string const& label)
{
  std::string out;
  out.reserve(label.size());

  for (char const c : label)
  {
    unsigned char const byte (static_cast<unsigned char>(c));

    bool const keep
      (  (byte >= 'a' && byte <= 'z')
      || (byte >= 'A' && byte <= 'Z')
      || (byte >= '0' && byte <= '9')
      || byte == '-'
      || byte == '_'
      || byte == '.'
      );

    if (keep)
    {
      out.push_back(static_cast<char>(byte));
      continue;
    }

    // Everything else becomes a single underscore however long the run was: path separators, the
    // colon that would otherwise open a drive-relative or alternate-data-stream name, the
    // wildcards a later glob or shell would expand, whitespace, control characters, and every
    // byte above 0x7F. Collapsing runs is what turns "tile  49  31" into "tile_49_31" rather
    // than "tile__49__31", and "../../evil" into ".._.._evil".
    if (out.empty() || out.back() != '_')
    {
      out.push_back('_');
    }
  }

  auto const is_separator = [] (char c) -> bool
  {
    return c == '_' || c == '-' || c == '.';
  };

  // Leading and trailing separators go, which is what finally disarms a traversal attempt:
  // ".._.._evil" leaves here as "evil". The trailing side matters for a second reason -- Windows
  // silently strips a trailing dot or space when creating a file, so the name on disk would stop
  // matching the one store() returned and load() would not find it.
  std::size_t begin (0);

  while (begin < out.size() && is_separator(out[begin]))
  {
    ++begin;
  }

  std::size_t end (out.size());

  while (end > begin && is_separator(out[end - 1]))
  {
    --end;
  }

  out = out.substr(begin, end - begin);

  if (out.size() > MAX_LABEL_LENGTH)
  {
    out.resize(MAX_LABEL_LENGTH);

    // Truncation can strand a separator at the end that was mid-word a moment ago. It cannot
    // empty the string: position 0 was already established not to be a separator.
    while (!out.empty() && is_separator(out.back()))
    {
      out.pop_back();
    }
  }

  // A label of nothing but punctuation, or an empty one, is answered with a default rather than
  // an exception. The label is provenance for a reviewer; a changeset is the thing that must not
  // be lost.
  return out.empty() ? std::string(DEFAULT_LABEL) : out;
}

std::string ChangesetArchive::readExactly
  (std::istream& stream, std::uintmax_t expected_bytes, std::string const& description)
{
  // Refused rather than truncated by the cast below. A 32-bit build asked for a five-gigabyte
  // changeset cannot answer, and answering with the low 32 bits of the length would be a silent
  // partial read wearing a success.
  if (expected_bytes >= static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max()))
  {
    throw ArchiveError
      ("The changeset backup " + description + " reports " + std::to_string(expected_bytes)
       + " bytes, which this process cannot hold in memory.");
  }

  std::size_t const wanted (static_cast<std::size_t>(expected_bytes));

  // One byte MORE than the file was measured to hold, and the read happens even when that
  // measurement was zero.
  //
  // The stat and the read are two operations and the file can change between them in EITHER
  // direction -- an archive on a share, a backup being rewritten by a second Noggit. Asking for
  // exactly `wanted` bytes detects only shrinkage: a file that grew came back quietly
  // truncated, which is the precise failure this check exists to rule out. One extra byte turns
  // growth into a loud refusal, and costs one byte.
  std::string buffer (wanted + 1u, '\0');

  stream.read(buffer.data(), static_cast<std::streamsize>(wanted) + 1);

  // gcount() rather than the stream's flags, and read() rather than `buffer << stream.rdbuf()`.
  // Reading one byte past the end of a correctly sized file sets eofbit and failbit, and that
  // is the SUCCESS case here; rdbuf() extraction sets failbit when it inserts nothing, which
  // would turn a legitimately empty changeset into a phantom I/O error.
  std::size_t const got (static_cast<std::size_t>(stream.gcount()));

  if (got > wanted)
  {
    throw ArchiveError
      ("The changeset backup " + description + " grew past the " + std::to_string(expected_bytes)
       + " bytes it measured while it was being read. Handing back the part that was read would"
       " be handing somebody a truncated changeset in the middle of a recovery, so it is"
       " refused; read it again.");
  }

  if (got < wanted)
  {
    throw ArchiveError
      ("Short read from the changeset backup " + description + ": got " + std::to_string(got)
       + " of " + std::to_string(expected_bytes) + " bytes.");
  }

  buffer.resize(wanted);

  return buffer;
}

bool ChangesetArchive::isMissing(std::error_code const& ec)
{
  // Compared against an errc rather than against a value. The same condition arrives in a
  // different category on each platform -- Win32 ERROR_FILE_NOT_FOUND and ERROR_PATH_NOT_FOUND
  // in std::system_category, POSIX ENOENT in std::generic_category -- and comparing an
  // error_code with an errc is the one operation that routes through the category's own mapping
  // and so answers the same question on both.
  return ec == std::errc::no_such_file_or_directory;
}

std::vector<ChangesetArchive::StoredFile> ChangesetArchive::collect() const
{
  requireRoot();

  std::vector<StoredFile> found;

  try
  {
    // Not recursive. The archive is one flat directory, and a .sql sitting in some subdirectory
    // of it was put there by somebody else -- reporting it as a backup this class made, and so
    // making it a candidate for prune(), would be a lie with consequences.
    for (std::filesystem::directory_entry const& entry
         : std::filesystem::directory_iterator(_root))
    {
      std::error_code ec;

      // symlink_status rather than status: a link is not followed, not reported and never
      // pruned. remove() on one would only unlink it, but presenting it as a stored changeset
      // would be a claim about where those bytes live that this class is in no position to make.
      std::filesystem::file_status const status (entry.symlink_status(ec));

      if (ec || status.type() != std::filesystem::file_type::regular)
      {
        continue;
      }

      // Kept as a path, never round-tripped through a narrow string. This is the value load()
      // and prune() are handed later, and .string() on a name the active code page cannot
      // express does not merely mangle it -- on MSVC it throws, from in here, which took the
      // whole listing down and with it store()'s sequence scan. See ArchiveEntry::file_name.
      // The ASCII view below is for the PARSER, which only ever reads the fixed-width prefix
      // and the extension, both of which store() writes as ASCII.
      std::filesystem::path file_name (entry.path().filename());

      ParsedName parsed;

      if (!parseName(asciiView(file_name), parsed))
      {
        continue;
      }

      std::uintmax_t const size (entry.file_size(ec));

      // A backup that went away between the directory scan and this stat is skipped rather than
      // fatal. A second Noggit pruning the same archive is enough to cause it, and failing the
      // whole listing would make store() -- which calls collect() to choose its next sequence
      // number -- refuse to archive a changeset that had nothing whatever wrong with it. prune()
      // already treats a file that is already gone this way, at its own remove() call.
      if (isMissing(ec))
      {
        continue;
      }

      if (ec)
      {
        throw ArchiveError
          ("Cannot size the changeset backup '" + describe(file_name) + "' in '" + _root.string()
           + "': " + ec.message()
           + ". Dropping it from the listing would hide a backup that exists.");
      }

      StoredFile stored;
      stored.entry.file_name = std::move(file_name);
      stored.entry.label = parsed.label;
      stored.entry.timestamp = parsed.timestamp;
      stored.entry.size_bytes = size;
      stored.sequence = parsed.sequence;

      found.push_back(std::move(stored));
    }
  }
  catch (ArchiveError const&)
  {
    throw;
  }
  catch (std::exception const& e)
  {
    // Iteration reports its own failures by throwing -- an unreadable root, a directory that
    // disappears under the iterator -- and a bad_alloc from a large listing lands here too.
    // Either way the caller gets this class's own exception type, so one catch covers the
    // archive whatever went wrong underneath.
    throw ArchiveError
      ("Cannot read the changeset archive '" + _root.string() + "': " + e.what());
  }

  return found;
}

std::string ChangesetArchive::nextFileName
  (std::uint64_t timestamp, std::string const& label) const
{
  std::string const stamp (stampFor(timestamp));

  // Sequence numbers are allocated across the whole second rather than per label, so that two
  // stores inside one second sort in the order they were made even when their labels differ.
  // "Text order is chronological order" is the one promise the naming scheme makes, and a
  // per-label sequence would break it for precisely the case the sequence exists to handle.
  long long highest (-1);

  for (StoredFile const& stored : collect())
  {
    if (stored.entry.timestamp == timestamp)
    {
      highest = std::max(highest, static_cast<long long>(stored.sequence));
    }
  }

  // Continuing past the highest in use rather than filling the first free slot: a number freed
  // by prune() would place a new changeset ahead of an older one in text order, and the whole
  // scheme is that ordering.
  for ( long long candidate (highest + 1)
      ; candidate <= static_cast<long long>(MAX_SEQUENCE)
      ; ++candidate
      )
  {
    std::string const file_name
      ( stamp + "-" + padded(static_cast<std::uint64_t>(candidate), SEQUENCE_LENGTH)
      + "-" + label + EXTENSION
      );

    std::error_code ec;

    // collect() only sees names it recognises, so a file may still be sitting on this exact name
    // under one that failed to parse -- an aborted copy, a rename by hand. Nothing is ever
    // overwritten, so ask.
    bool const taken (std::filesystem::exists(_root / file_name, ec));

    if (ec)
    {
      throw ArchiveError
        ("Cannot tell whether '" + file_name + "' already exists in '" + _root.string() + "': "
         + ec.message());
    }

    if (!taken)
    {
      return file_name;
    }
  }

  throw ArchiveError
    ("The changeset archive '" + _root.string() + "' already holds "
     + std::to_string(MAX_SEQUENCE + 1) + " changesets stamped " + stamp + ", and the sequence"
     " field cannot widen without breaking the ordering of every other name. Stamp the next"
     " batch a second later.");
}

void ChangesetArchive::requireRoot() const
{
  std::error_code ec;

  if (!std::filesystem::is_directory(_root, ec))
  {
    throw ArchiveError
      ("The changeset archive root '" + _root.string() + "' is not a directory"
       + (ec ? ": " + ec.message() : std::string(".")) + " Reporting an unreachable archive as an"
       " empty one would hide every backup it holds, so this is an error and not a quiet no-op.");
  }
}

void ChangesetArchive::ensureRoot()
{
  std::error_code ec;

  if (std::filesystem::is_directory(_root, ec))
  {
    return;
  }

  std::filesystem::create_directories(_root, ec);

  // Confirmed by asking the filesystem again rather than by trusting the return value:
  // create_directories answers false both when the directory already existed -- a race with
  // another Noggit process, which is fine -- and when it could not be created, which is not,
  // and implementations differ over whether the harmless case also sets an error code.
  std::error_code check;

  if (!std::filesystem::is_directory(_root, check))
  {
    throw ArchiveError
      ("Cannot create the changeset archive root '" + _root.string() + "': " + ec.message()
       + ". The changeset has NOT been archived.");
  }
}
