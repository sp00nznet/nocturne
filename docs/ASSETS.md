# Phase 2 — The POD archives

Nocturne keeps everything it loads in 41 `.POD` archives holding **12,383 files**.
`tools/pod.py` reads them; nothing here is committed to the repo.

## Two formats, not one

One of the assert strings recovered in Phase 0 is `Invalid pod version!` — a hint
that `CPodFile` handles more than one layout. It does:

* **40 archives are POD2** — magic `POD2`, a name table, per-file timestamps and
  checksums, and a trailing audit block.
* **`tground.pod` alone is POD1** — no magic at all, fixed 32-byte names, no
  checksums. This is the same layout Fury³ and Hellbender use, so the ground-type
  archive is a survivor from the studio's previous engine that nobody ever
  re-packed.

Both are documented in the header comment of `tools/pod.py` and covered by its
`--selftest`.

```bash
py -3 tools/pod.py --selftest                       # round-trips both formats
py -3 tools/pod.py list    _game/Nocturne/music.pod
py -3 tools/pod.py audit   _game/Nocturne/hero.pod
py -3 tools/pod.py extract _game/Nocturne/STARTUP.POD _extracted/startup
```

Because POD1 has no magic, it is identified by elimination — anything that isn't
`POD2` is parsed as POD1, and a directory that overruns the file is the error.

## The audit block

POD2 archives end with an array of 312-byte audit records: a username, a
timestamp, the asset path, and the file's own size and timestamp. This is a
build-log — the check-in history of the archive, written by Terminal Reality's
packing tool and shipped to customers by accident of format. It is what
`CPodFile::getAuditRecord` walks, and it is not needed to read a single asset.

It is, however, a remarkable historical artifact: **32,978 records across the 40
POD2 archives, from 10 distinct accounts**, dating from May 1999 to the November
build. `hero.pod`'s audit block shows the player character's skeleton
(`DATA\STRANGER.SKL`) revised four times in two days in May 1999, and an account
named `MODELBOT` checking in texture/palette pairs in batches — an automated
export step, preserved in the shipping archive.

## What's in there

| Ext | Count | Bytes | Notes | Original reader |
|---|---:|---:|---|---|
| `.RAW` | 3,157 | 554 MB | 8-bit texture bitmaps | `engine\texture.cpp` |
| `.ACT` | 3,115 | 2.4 MB | palettes — **always exactly 768 bytes** (256×RGB) | `engine\texture.cpp` |
| `.FOG` | 1,611 | 56 MB | per-texture fog/shade tables | `engine\texture.cpp` |
| `.SFX` | 1,229 | 21 KB | sound descriptors, ~17 bytes each | `sound\sndmain.cpp` |
| `.MP3` | 1,004 | 27 MB | speech | `sound\mp3.cpp` |
| `.WAV` | 860 | 165 MB | music and effects | `sound\snddx.cpp` |
| `.KFM` | 577 | 28 MB | keyframed models | `engine\keyframe.cpp` |
| `.OPA` | 398 | 12 MB | — | — |
| `.DFM` | 113 | 30 MB | deformable models | `CDeformableModel` |
| `.SKL` | 70 | 80 MB | skeletons | `core\skeleton.cpp` |
| `.MSN` | 28 | 7.3 MB | mission definitions | `core\mission.cpp` |
| `.SCR` | 28 | 453 KB | scripts | `core\script.cpp` |
| `.CTH` | 25 | 13 KB | cloth | `core\cloth.cpp` |
| `.GEO` | 20 | 154 MB | level geometry, ~7.7 MB each | `core\set.cpp` |
| `.THM` | 20 | 61 MB | **always exactly 3,072,000 bytes** | — |
| `.ZTH` | 20 | 20 MB | — | — |
| `.SET` | 20 | 2.6 MB | level sets | `core\set.cpp`, `core\setutil.cpp` |
| `.PTH` | 17 | 663 KB | navigation paths | `core\path.cpp` |
| `.VOX` | 10 | 2.8 KB | subtitle/voice index | — |
| `.MOV` | 3 | 37 MB | in-engine movies | `core\filmreel.cpp` |
| `.CL0/1/2` | 3 | 1.8 MB | fixed-size colour lookups | — |

The extension list and the 99 source filenames recovered from the assert strings
line up almost one-to-one — `.KFM`↔`keyframe.cpp`, `.SKL`↔`skeleton.cpp`,
`.CTH`↔`cloth.cpp`, `.PTH`↔`path.cpp`, `.MSN`↔`mission.cpp`, `.SET`↔`set.cpp`.
That mutual confirmation is worth more than either piece of evidence alone: it
says the module map is real and tells us which lifted functions to look at first
when a given format needs decoding.

## Archive layout

Archives are split by act and by level, which maps directly onto the game's
structure: `STARTUP.POD` (1,541 files, comment *"Required files for Nocturne"*)
plus `ENGLISH.POD` (2,032 — all speech and text), `SOUND.POD`, `music.pod`, and
then one archive per location — `mansion` (532 files), `chicago` (658), `castle`,
`forest`, `grave`, `mine`, `temple`, `theater`, `town`, `train`, `dungeon`,
`factory`, and so on — with `hero.pod`, `enemy.pod` (*"Our malicious enemies"*)
and `npc.pod` holding the characters. The comments are the developers' own labels
and are worth reading; `hero.pod`'s is *"Our valliant heroes"*, typo included.

## What this unlocks

The engine's file layer is now readable from outside the engine. That matters for
bring-up in two specific ways:

1. **Ground truth for the lifted loader.** When lifted `CPodFile::mount` runs, we
   can diff what it thinks the archive contains against what `tools/pod.py` says
   it contains, which turns a class of silent asset failures into an assertion.
2. **Formats can be decoded before the engine runs.** `.ACT` is already solved
   (768-byte palette). `.RAW` + `.ACT` + `.FOG` triples are the texture pipeline,
   and that is exactly the data the 37-call renderer interface consumes via
   `APIDLLupdateTexture` / `APIDLLselectTexture` / `APIDLLsetColorTable16`. The
   renderer work in Phase 6 can be developed and tested against real textures
   without a single lifted function running.
