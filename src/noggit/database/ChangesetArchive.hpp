// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DATABASE_CHANGESETARCHIVE_HPP
#define NOGGIT_DATABASE_CHANGESETARCHIVE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace Noggit::Database
{
  // Raised when the archive cannot do what it was asked.
  //
  // Every failure path here throws, including ones another component might reasonably shrug
  // off. A backup that quietly did not happen is worse than no backup at all: the user stops
  // keeping their own copy of a changeset the moment they believe the tool is keeping one, so
  // the only acceptable outcomes are "stored" and "told loudly".
  class ArchiveError : public std::runtime_error
  {
    public:
      explicit ArchiveError(std::string const& message)
        : std::runtime_error(message) {}
  };

  // One stored changeset, as reported by ChangesetArchive::list().
  struct ArchiveEntry
  {
    // A single path component, never a directory. Hand it straight back to load().
    //
    // A std::filesystem::path and not a std::string, because a narrow string is not a spelling
    // of a Windows file name at all: path::string() converts through the active code page. One
    // backup copied in from a machine that uses another encoding -- an unremarkable file name
    // there -- was enough to break the whole archive, and measurably worse than the "name comes
    // back with replacement characters" this was first recorded as. On MSVC 19.4x with code
    // page 1252 the conversion THROWS ("No mapping for the Unicode character exists in the
    // target multi-byte code page"), so list() threw, prune() threw, and store() threw too,
    // since it calls the same directory scan to choose its next sequence number. Noggit could
    // not archive anything at all while that one file sat in the root.
    std::filesystem::path file_name;

    // The SANITISED label, decoded from the file name, for display. Not necessarily the string
    // that was passed to store() -- see ChangesetArchive::sanitiseLabel.
    //
    // A rendering and not a handle: every non-ASCII code unit of a foreign name appears here as
    // '?', because the label is text for a human while file_name is the thing that opens.
    std::string label;

    // Seconds since the Unix epoch, exactly the value handed to store().
    //
    // Decoded from the file name rather than read from the filesystem, so it survives the
    // archive being copied, zipped or moved between machines. A last-write-time would not, and
    // "the backup is from some time on Tuesday, probably" is not a recovery story.
    std::uint64_t timestamp = 0;

    std::uintmax_t size_bytes = 0;
  };

  // Timestamped, restorable backups of every emitted changeset.
  //
  // The milestone requirement is narrow and worth restating exactly: a changeset the user has
  // deleted must still be recoverable. Everything in this class follows from that one sentence.
  //
  //   - Nothing is ever overwritten. A name that is already taken gets the next sequence number
  //     instead of replacing what is there, so no store() can destroy an earlier one.
  //   - Nothing is deleted except by prune(), and prune() only ever deletes files this class
  //     both wrote and can still recognise.
  //   - A partially written file is left on disk rather than cleaned up, and named in the
  //     exception. Half a changeset a human can inspect beats none.
  //
  // File names look like this:
  //
  //   <YYYYMMDD>-<HHMMSS>-<NNN>-<label>.sql       20260801-160000-000-tile_49_31.sql
  //
  // Everything before the label is fixed width, zero padded and UTC, which makes a plain
  // lexicographic sort of the names a chronological sort. That is the only promise the naming
  // scheme makes, and it is what lets list(), prune() and a directory listing in any file
  // manager agree with one another for free.
  //
  // UTC rather than local time for two reasons: an hour repeats every autumn in any zone with
  // daylight saving, which would put two names out of order for a whole hour once a year; and
  // two archives from machines in different zones cannot be merged and read in order at all.
  //
  // <NNN> is a per-second sequence number, allocated across the whole second rather than per
  // label, so a burst of stores inside one second still sorts in the order it happened.
  //
  // Uses std::filesystem, no database and no Qt, so it is testable on a bare machine like the
  // rest of this layer.
  class ChangesetArchive
  {
    public:
      // Longest sanitised label that survives into a file name.
      //
      // The full name is then at most 20 + 48 + 4 = 72 characters, which leaves room for a
      // deeply nested archive root under the legacy 260-character Windows path limit. A label
      // is provenance for a reviewer, not a description; anything longer is a comment inside
      // the .sql.
      static constexpr std::size_t MAX_LABEL_LENGTH = 48;

      // The root is explicit and mandatory. Nothing here derives a location from the
      // environment: a backup directory the user did not choose is one they will not find.
      //
      // Throws ArchiveError on an empty path, which would otherwise resolve relative to the
      // process working directory and scatter changesets wherever Noggit happened to be
      // started from. Refused at construction rather than at first store(), so the
      // misconfiguration cannot survive long enough to matter.
      explicit ChangesetArchive(std::filesystem::path root);

      std::filesystem::path const& root() const { return _root; }

      // Writes `sql` verbatim and returns the path written.
      //
      // `timestamp` is seconds since the Unix epoch. It is a PARAMETER rather than a clock read
      // so that ordering, collisions and the whole naming scheme are testable without waiting
      // for real time to pass, and so a caller batching several changesets can stamp them all
      // with one instant.
      //
      // Creates the root if it is absent. That is the one place this class prefers doing the
      // work to complaining: refusing to store because a directory does not exist yet would
      // lose exactly the file it exists to keep.
      //
      // Throws ArchiveError when the root cannot be created or is not a directory, when the
      // write fails or comes up short, when a thousand changesets already share the second, or
      // when `timestamp` is past 9999-12-31T23:59:59Z -- which in practice means milliseconds
      // were passed where seconds were expected.
      std::filesystem::path store
        ( std::string const& sql
        , std::string const& label
        , std::uint64_t timestamp
        );

      // Every stored changeset, newest first.
      //
      // Only files this class recognises are reported: directly in the root, not through a
      // symbolic link, and named to the scheme above. A hand-written .sql or a stray note in
      // the archive directory is not a backup this class made and is not presented as one --
      // which is also what keeps prune() from deleting it.
      //
      // Throws ArchiveError when the root is absent or unreadable. An unreadable archive
      // reported as an empty one is the failure this class is least allowed to have.
      std::vector<ArchiveEntry> list() const;

      // The exact bytes of a stored changeset.
      //
      // `file_name` must be a bare name as returned by list(). Throws ArchiveError when it
      // names a directory component, a root, "." or ".." -- a label is caller-supplied text and
      // a name derived from one must never be able to reach outside the archive -- and when the
      // file is absent, unreadable, or changes length while it is being read.
      //
      // A std::string or a string literal still converts implicitly, with one caveat worth
      // stating: on Windows that conversion is what rejects a narrow string the active code
      // page cannot express, and it does so by throwing std::filesystem::filesystem_error at
      // the CALL SITE rather than ArchiveError from in here. Callers that hand back a
      // file_name from list() never meet it, since that value is already a path.
      std::string load(std::filesystem::path const& file_name) const;

      // Deletes all but the newest `keep_newest` changesets and returns how many were removed.
      //
      // keep_newest == 0 empties the archive of everything this class wrote; a value at or
      // above the current count removes nothing and returns 0.
      //
      // Only files list() reports are candidates, so nothing outside the root, nothing behind a
      // symbolic link and nothing another tool put there can be deleted. Throws ArchiveError if
      // a deletion fails, reporting how many had already gone so the caller knows the archive
      // is in a partially pruned state.
      std::size_t prune(std::size_t keep_newest);

      // The label as it will appear in a file name.
      //
      // Public because it is half the security boundary of this class and deserves to be
      // tested and reasoned about directly. Every byte outside [A-Za-z0-9._-] becomes an
      // underscore, runs collapse to one, leading and trailing separators are dropped and the
      // result is capped at MAX_LABEL_LENGTH. An empty result becomes a fixed default, because
      // substituting a name cannot lose a changeset and refusing to store one can.
      //
      // Bytes above 0x7F are replaced rather than kept: on Windows a narrow std::string reaching
      // std::filesystem::path is interpreted in the active code page, not as UTF-8, so a UTF-8
      // label written through would produce a mojibake file name that no longer matches the
      // bytes the caller passed. Payloads are UTF-8 safe -- they are written verbatim -- but
      // file names are deliberately ASCII.
      static std::string sanitiseLabel(std::string const& label);

      // The whole of `expected_bytes` from `stream`, or an ArchiveError naming `description`.
      //
      // A file has to be measured before it is read, and the two are separate operations: it
      // can change in EITHER direction in between. Reading exactly `expected_bytes` notices
      // only shrinkage, so a file that GREW came back silently truncated -- the one failure
      // this class must never have, given that its whole purpose is that a lost changeset comes
      // back whole. One byte is read past the measured end so that growth is as loud as a short
      // read.
      //
      // Public for the same reason sanitiseLabel is: it carries the whole of the guarantee that
      // what load() returns is an entire file, and the case it exists for cannot be STAGED
      // through load(). Making a real file change length between the stat and the read needs a
      // second writer and a won race, and a test that has to win a race is a test that will
      // one day report a fix that is not there. A stream holding more or fewer bytes than it
      // claims asks the same question with no timing in it at all.
      static std::string readExactly
        ( std::istream& stream
        , std::uintmax_t expected_bytes
        , std::string const& description
        );

      // True when a per-file error means the file is simply not there any more.
      //
      // Public for the same reason again: it decides alone whether one directory entry is
      // skipped or the whole archive is declared unreadable, and it rests on a mapping that
      // deserves a test rather than a comment -- Win32 ERROR_FILE_NOT_FOUND arrives in
      // std::system_category while POSIX ENOENT arrives in std::generic_category, and the skip
      // only happens on both platforms because comparing an error_code against std::errc goes
      // through the category's own mapping rather than through the raw value.
      static bool isMissing(std::error_code const& ec);

    private:
      // A listed file, plus the sequence number decoded from its name. The sequence is an
      // implementation detail of the naming scheme, so it does not appear in ArchiveEntry.
      struct StoredFile
      {
        ArchiveEntry entry;
        unsigned int sequence = 0;
      };

      // Unsorted. Shared by list() and the next-name allocation in store(), so both agree
      // exactly on which files belong to this archive.
      std::vector<StoredFile> collect() const;

      std::string nextFileName(std::uint64_t timestamp, std::string const& label) const;

      void requireRoot() const;
      void ensureRoot();

      std::filesystem::path _root;
  };
}

#endif
