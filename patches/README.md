# Patches against submodules

`src/external/blizzard-archive-library` is a submodule pointing at
`gitlab.com/T1ti/blizzard-archive-library`, which this project cannot push to. The parent
repository records only that submodule's **commit pointer**, so a fix made inside it works on the
machine that made it and does not exist for anyone who clones.

Anything fixed there is therefore also exported here as a patch file, so the change is visible,
reviewable, and reapplicable.

## This is a REQUIRED build step

The parent repository deliberately records the submodule at its last **public** commit
(`6cac330`), not at a local commit containing the fix. A parent recording a SHA that exists on no
remote makes `git submodule update --init --recursive` fail for everyone except the machine that
made it — the repo would not build at all for anyone who cloned it.

So the patch below must be applied after checking out submodules, before building. Without it,
**Client ▸ Patch Client produces archives the game client silently ignores.**

## Applying

```
cd src/external/blizzard-archive-library
git am ../../../patches/0001-blizzard-archive-mpq-backslash-names.patch
```

Afterwards `git status` in the parent reports that submodule as modified. **That is the intended
steady state** — leave it. Never commit the resulting pointer.

## Maintainers: verify the recorded pointer before every release

The pointer is easy to re-record by accident, because `git commit -a` and most "commit everything"
UI actions sweep up a dirty submodule along with real changes. If that happens, the published
repository records a commit that exists on no remote and **`git submodule update --init
--recursive` fails for every person who clones it.**

```
git ls-tree HEAD src/external/blizzard-archive-library
```

That must print the last public commit, `6cac3304b272ee6688e88573057e94e085d35caf`. If it prints
anything else — in particular `f2b1716…`, the local commit this patch was exported from — reset it
before publishing:

```
git update-index --cacheinfo 160000,6cac3304b272ee6688e88573057e94e085d35caf,src/external/blizzard-archive-library
```

then commit that change alone. Your working tree keeps the applied patch either way.

## 0001 — MPQ entry names must use backslashes

Without this, **every archive written by Client > Patch Client is silently ignored by the game
client.**

MPQ resolves a file by hashing its name, so a stored name has to be byte-for-byte what the client
asks for — `World\Maps\Expansion01\Expansion01_29_32.adt`. `Listfile::FileKey` normalises to
forward slashes for Noggit's own lookups, and that normalised form was passed straight to
`SFileAddFileEx`, so every file was filed under a name nothing would ever request.

What made it hard to find: the archive is otherwise perfect. Valid v1 header, correct payload
(the ADTs were byte-identical to the project copies), correct archive name, correct internal
paths. And every MPQ viewer lists the contents normally, because viewers read the `(listfile)` rather
than hashing. The only symptom is that nothing happens in game.

Diagnosed by comparing a known-working custom patch against a Noggit-written one: the working
archive stored `Creature\beemount\beemount.m2`, Noggit stored `world/maps/...`. Headers were
otherwise identical.

Confirmed working in game against a 3.3.5a client.
