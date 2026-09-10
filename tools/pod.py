#!/usr/bin/env python3
"""
pod.py - Reader for Terminal Reality POD archives (Nocturne's asset format).

Nocturne ships 41 .POD files holding everything the engine loads. The reader in
the original binary is `engine\\pod.cpp`, class `CPodFile` -- its assert strings
name the CRC path (`computeOneFileCRC`, `getAuditRecord`), which is what told us
the trailing audit block existed before we went looking for it, and one of them
is literally `Invalid pod version!`, which is the hint that there is more than
one version in play. There is: 40 archives are POD2, and `tground.pod` alone is
the older POD1, the same layout Fury3 and Hellbender use.

POD1 layout, all little-endian -- no magic, so it is identified by elimination:

    0    u32   fileCount
    4    char  comment[80]
    84   entry[fileCount]    40 bytes each:
             char name[32]
             u32  size
             u32  offset
         file data

POD2 layout, all little-endian:

    0    char  magic[4]      "POD2"
    4    u32   checksum      whole-archive checksum
    8    char  comment[80]   e.g. "Required files for Nocturne"
    88   u32   fileCount
    92   u32   auditCount
    96   entry[fileCount]    20 bytes each:
             u32 nameOffset  byte offset into the name table
             u32 size
             u32 offset      absolute, from start of archive
             u32 timestamp   unix time
             u32 checksum
         name table          NUL-terminated paths, back-slashed
         file data
         audit[auditCount]   312 bytes each, at the end of the archive:
             char user[32]   the Terminal Reality dev who checked the file in
             u32  timestamp
             u32  action
             char path[264]
             u32  fileTimestamp
             u32  fileSize

The audit block is a build-log artifact -- it is not needed to read assets, but
it is what `CPodFile::getAuditRecord` walks, and it is a genuinely nice piece of
1999 studio history, so it is parsed rather than skipped.

    py -3 tools/pod.py list    <pod>            # contents
    py -3 tools/pod.py audit   <pod>            # who checked what in, and when
    py -3 tools/pod.py extract <pod> <out-dir>  # unpack (bounds-checked)
"""
import os
import struct
import sys
from datetime import datetime, timezone

MAGIC = b"POD2"
HEADER = 96
ENTRY = 20
AUDIT = 312

P1_HEADER = 84
P1_ENTRY = 40


class PodError(Exception):
    pass


class Pod:
    def __init__(self, path):
        self.path = path
        self.data = open(path, "rb").read()
        self.version = 2 if self.data[:4] == MAGIC else 1
        if self.version == 2:
            self._parse_pod2()
        else:
            self._parse_pod1()

    def _parse_pod2(self):
        d = self.data
        self.checksum = struct.unpack_from("<I", d, 4)[0]
        self.comment = d[8:88].split(b"\x00")[0].decode("latin1")
        self.file_count, self.audit_count = struct.unpack_from("<II", d, 88)

        dir_end = HEADER + self.file_count * ENTRY
        if dir_end > len(d):
            raise PodError("%s: directory runs past end of file" % self.path)
        names = d[dir_end:]

        self.files = []
        for i in range(self.file_count):
            n_off, size, off, ts, crc = struct.unpack_from(
                "<IIIII", d, HEADER + i * ENTRY)
            end = names.find(b"\x00", n_off)
            if end < 0:
                raise PodError("%s: entry %d has an unterminated name"
                               % (self.path, i))
            self._check_bounds(i, off, size)
            self.files.append({
                "name": names[n_off:end].decode("latin1"),
                "size": size, "offset": off, "timestamp": ts, "checksum": crc,
            })

    def _parse_pod1(self):
        d = self.data
        self.checksum = 0
        self.audit_count = 0
        self.file_count = struct.unpack_from("<I", d, 0)[0]
        self.comment = d[4:84].split(b"\x00")[0].decode("latin1")

        if P1_HEADER + self.file_count * P1_ENTRY > len(d):
            raise PodError("%s: not a POD1 archive either (directory overruns)"
                           % self.path)
        self.files = []
        for i in range(self.file_count):
            base = P1_HEADER + i * P1_ENTRY
            name = d[base:base + 32].split(b"\x00")[0].decode("latin1")
            size, off = struct.unpack_from("<II", d, base + 32)
            self._check_bounds(i, off, size)
            self.files.append({
                "name": name, "size": size, "offset": off,
                "timestamp": 0, "checksum": 0,
            })

    def _check_bounds(self, i, off, size):
        if off + size > len(self.data):
            raise PodError("%s: entry %d data runs past end of file"
                           % (self.path, i))

    def read(self, entry):
        return self.data[entry["offset"]:entry["offset"] + entry["size"]]

    def audit(self):
        """Audit records live immediately after the last byte of file data."""
        if not self.audit_count or not self.files:
            return []
        base = max(f["offset"] + f["size"] for f in self.files)
        out = []
        for i in range(self.audit_count):
            r = self.data[base + i * AUDIT: base + (i + 1) * AUDIT]
            if len(r) < AUDIT:
                break
            ts, action = struct.unpack_from("<II", r, 32)
            f_ts, f_size = struct.unpack_from("<II", r, 304)
            out.append({
                "user": r[:32].split(b"\x00")[0].decode("latin1"),
                "timestamp": ts, "action": action,
                "path": r[40:304].split(b"\x00")[0].decode("latin1"),
                "file_timestamp": f_ts, "file_size": f_size,
            })
        return out


def when(ts):
    if not ts:
        return "-"
    return datetime.fromtimestamp(ts, timezone.utc).strftime("%Y-%m-%d")


def safe_rel(name):
    """Archive paths are back-slashed and relative. Refuse anything that would
    escape the output directory rather than trusting 26-year-old data."""
    rel = os.path.normpath(name.replace("\\", "/"))
    if os.path.isabs(rel) or rel == ".." or rel.startswith(".." + os.sep):
        raise PodError("refusing unsafe path %r" % name)
    return rel


def cmd_list(pod):
    print("%s: POD%d, %d files, %d audit records, comment %r"
          % (pod.path, pod.version, pod.file_count, pod.audit_count, pod.comment))
    for f in pod.files:
        print("  %10d  %s  %s" % (f["size"], when(f["timestamp"]), f["name"]))


def cmd_audit(pod):
    print("%s: %d audit records" % (pod.path, pod.audit_count))
    for a in pod.audit():
        print("  %-12s %s  %10d  %s"
              % (a["user"], when(a["timestamp"]), a["file_size"], a["path"]))


def cmd_extract(pod, outdir):
    n = 0
    for f in pod.files:
        dest = os.path.join(outdir, safe_rel(f["name"]))
        os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
        with open(dest, "wb") as fh:
            fh.write(pod.read(f))
        n += 1
    print("%s: extracted %d files -> %s" % (os.path.basename(pod.path), n, outdir))


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    cmd, path = argv[0], argv[1]
    pod = Pod(path)
    if cmd == "list":
        cmd_list(pod)
    elif cmd == "audit":
        cmd_audit(pod)
    elif cmd == "extract":
        if len(argv) < 3:
            print("extract needs an output directory", file=sys.stderr)
            return 2
        cmd_extract(pod, argv[2])
    else:
        print("unknown command %r" % cmd, file=sys.stderr)
        return 2
    return 0


def _selftest():
    """Round-trip the header/entry/audit packing this module claims to read."""
    import tempfile

    files = [("ART\\VGA.ACT", b"palette-bytes"), ("SOUND\\CUE1.WAV", b"RIFF....")]
    names, name_offs = b"", []
    for n, _ in files:
        name_offs.append(len(names))
        names += n.encode("latin1") + b"\x00"
    data_off = HEADER + len(files) * ENTRY + len(names)
    blob, entries, off = b"", [], data_off
    for (n, payload), n_off in zip(files, name_offs):
        entries.append(struct.pack("<IIIII", n_off, len(payload), off, 0x37B8AEE4, 0xDEAD))
        blob += payload
        off += len(payload)
    audit = (b"jeffm".ljust(32, b"\x00") + struct.pack("<II", 0x37BC8481, 0)
             + b"ART\\VGA.ACT".ljust(264, b"\x00") + struct.pack("<II", 0x37B8AEE4, 13))
    assert len(audit) == AUDIT, len(audit)
    raw = (MAGIC + struct.pack("<I", 0x1234) + b"unit test".ljust(80, b"\x00")
           + struct.pack("<II", len(files), 1) + b"".join(entries) + names + blob + audit)

    with tempfile.TemporaryDirectory() as td:
        p = os.path.join(td, "t.pod")
        open(p, "wb").write(raw)
        pod = Pod(p)
        assert pod.comment == "unit test"
        assert [f["name"] for f in pod.files] == [n for n, _ in files]
        assert [pod.read(f) for f in pod.files] == [b for _, b in files]
        a = pod.audit()
        assert len(a) == 1 and a[0]["user"] == "jeffm"
        assert a[0]["path"] == "ART\\VGA.ACT" and a[0]["file_size"] == 13
        cmd_extract(pod, os.path.join(td, "out"))
        got = open(os.path.join(td, "out", "ART", "VGA.ACT"), "rb").read()
        assert got == b"palette-bytes", got
        pod.files[0]["name"] = "..\\..\\evil.txt"
        try:
            cmd_extract(pod, os.path.join(td, "out2"))
        except PodError:
            pass
        else:
            raise AssertionError("traversal path was not refused")

        # POD1: no magic, fixed 32-byte names, no audit block.
        p1_entries, p1_blob, off = b"", b"", P1_HEADER + len(files) * P1_ENTRY
        for n, payload in files:
            p1_entries += n.encode("latin1").ljust(32, b"\x00")
            p1_entries += struct.pack("<II", len(payload), off)
            p1_blob += payload
            off += len(payload)
        p1 = os.path.join(td, "t1.pod")
        open(p1, "wb").write(struct.pack("<I", len(files))
                             + b"legacy".ljust(80, b"\x00") + p1_entries + p1_blob)
        old = Pod(p1)
        assert old.version == 1 and old.comment == "legacy"
        assert [f["name"] for f in old.files] == [n for n, _ in files]
        assert [old.read(f) for f in old.files] == [b for _, b in files]
        assert old.audit() == []
    print("pod.py self-test OK")


if __name__ == "__main__":
    if sys.argv[1:2] == ["--selftest"]:
        _selftest()
    else:
        sys.exit(main(sys.argv[1:]))
