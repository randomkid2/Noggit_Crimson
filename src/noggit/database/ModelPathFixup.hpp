// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_DATABASE_MODELPATHFIXUP_HPP
#define NOGGIT_DATABASE_MODELPATHFIXUP_HPP

#include <cstddef>
#include <string>

namespace Noggit::Database
{
  // Turns the model name a DBC (or an MDDF) carries into the key the MPQ chain can actually
  // open.
  //
  // The client's data files are .m2. Every *reference* to them in DBC string tables and in ADT
  // MMDX blocks is .mdx or .mdl, a leftover from the pre-release model format. Something has to
  // rewrite the extension, and this project already does it in six places, none of them
  // reusable and three of them wrong:
  //
  //   - ClientData::normalizeFilenameInternal (ClientData.cpp:679) is the closest thing to a
  //     shared helper and is what Listfile::FileKey applies to every path. It does the rewrite
  //     with std::regex_replace(path, std::regex(".mdx"), ".m2"). The '.' is an unescaped regex
  //     wildcard and regex_replace replaces EVERY match, so "world/amdx/foo.mdx" becomes
  //     "world/.m2/foo.m2" -- a directory name silently destroyed. The ends_with(".mdx") guard
  //     on the line above does not prevent it: it only decides whether the replace runs, not
  //     how many matches it then eats.
  //   - WMO.cpp:180 hand-rolls a while-loop replacing every literal "mdx" anywhere in the path,
  //     with the same consequence and no extension anchor at all.
  //   - ModelManager.cpp:10 (file-local, anonymous namespace) is correct: rfind(".mdx") then a
  //     4-byte replace. It is also unreachable from here and case-sensitive, so it misses ".MDX".
  //   - ObjectEditor.cpp:941 is the same correct rfind/replace open-coded a second time, and
  //     handles only .mdx -- a WMV log naming a .mdl reaches the model loader unrewritten.
  //   - GroundEffectsTool.cpp:785 and ChunkAddDetailDoodads.cpp:337 use QString::replace with
  //     Qt::CaseInsensitive -- correct on case, unanchored on position, and Qt-dependent.
  //
  // So this is not a duplicate of a working helper; it is the first anchored, case-insensitive,
  // Qt-free one. It is deliberately free of DBC, Qt, glm and SQL so it stays in the test target
  // that runs without a toolchain, because it is the one part of display resolution that can be
  // exhaustively tested with no client data present.
  namespace ModelPathFixup
  {
    // Longest path this module will accept, matching Windows MAX_PATH.
    //
    // Exported rather than kept internal because DisplayResolver needs the same bound: it reads
    // model names through DBCFile::Record::getString, which returns a raw pointer into the
    // string table validated by an assert() that RelWithDebInfo compiles out. A caller with an
    // untrusted pointer needs a length cap to scan to, and it must be the same cap this module
    // rejects on, or a name could survive the read and then be silently dropped here.
    inline constexpr std::size_t MAX_MODEL_PATH_LENGTH = 260;

    // Rewrites a DBC/MDDF model name into an MPQ-openable .m2 key.
    //
    // In order: trims surrounding whitespace, rejects bytes that cannot appear in a model path,
    // normalises '/' to '\\', collapses separator runs, drops leading separators, then rewrites
    // a trailing .mdx/.mdl (any case) to .m2. An extension that is already .m2 passes through.
    //
    // Returns an EMPTY STRING for anything that cannot be a model path -- no extension, a
    // non-model extension, a bare extension with no name, a '.' or '..' component, or a name
    // over MAX_MODEL_PATH_LENGTH. Guessing is worse than failing here: a wrong key produces a
    // "file not found" from deep inside the archive layer, attributed to the model loader
    // rather than to the DBC row that is actually malformed.
    //
    // Separators come out as backslash, and the extension always comes out lowercase, so the
    // result has exactly one spelling and is safe to use as a map key or a dedup key. Note that
    // the rest of the name keeps its original case on purpose: case folding is
    // ClientData::normalizeFilenameInternal's job, it is applied again by Listfile::FileKey's
    // constructor, and doing it here would mean calling ::tolower on bytes >= 0x80, which is
    // locale-sensitive and undefined for a signed char.
    std::string toM2Path(std::string const& dbc_name);

    // True when toM2Path would produce something usable.
    //
    // Defined in terms of toM2Path rather than repeating its rules, so the two cannot drift into
    // disagreeing about what a model path is -- which would show up as a spawn that passes the
    // check and then resolves to nothing.
    bool looksLikeModelPath(std::string const& path);
  }
}

#endif
