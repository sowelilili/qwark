#!/usr/bin/env python3
"""
buildid.py: the module's build ID, and the qwark.build file that describes a
built module to the PC client (revision 1.24, PROTOCOL.md 3.1.1).

Between two releases every test build reports the same QWARK_BUILD, so the
build number cannot tell the client that the module on a console is not the one
it ships. The build ID can: the CRC-32 of the module's sources, every .c and .h
under src/ (not the helpers' out/ folders) and the makefile, each preceded by
its path, with carriage returns dropped so a checkout's line endings do not
count. The same sources give the same ID on any machine and in any build, so a
rebuild of what is committed matches it, and any change to them gives another.
0 means "no ID" on the wire, so a CRC of 0 is reported as 1.

    python -I tools/buildid.py                  prints the ID, 0x%08X
    python -I tools/buildid.py --dev 1 --out F  writes F, the sidecar `make`
        [--id 0x...]                            puts beside qwark.sprx, for the
                                                ID the module was built with

The sidecar is three lines of text, `build N`, `dev 0|1` and `id 0x...`, after
a comment line; `make dist` copies it into dist/ and the client's release
carries it beside qwark.sprx. Needs no SDK; Python, as pak.py.
"""
import argparse
import os
import re
import sys
import zlib

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))


def sources(root):
    """The files the ID covers, as sorted repo-relative paths with forward slashes."""
    found = []
    for d, dirs, names in os.walk(os.path.join(root, "src")):
        dirs[:] = [x for x in dirs if x != "out"]
        for n in names:
            if n.endswith((".c", ".h")):
                found.append(os.path.relpath(os.path.join(d, n), root).replace(os.sep, "/"))
    found.sort()
    found.append("makefile")
    return found


def build_id(root=ROOT):
    crc = 0
    for rel in sources(root):
        with open(os.path.join(root, rel), "rb") as f:
            data = f.read().replace(b"\r", b"")
        crc = zlib.crc32(rel.encode("ascii") + b"\0", crc)
        crc = zlib.crc32(data, crc)
    crc &= 0xFFFFFFFF
    return crc if crc != 0 else 1


def qwark_build(root=ROOT):
    path = os.path.join(root, "src", "core", "proto.h")
    m = re.search(r"^#define\s+QWARK_BUILD\s+(\d+)", open(path, encoding="utf-8").read(), re.M)
    if not m:
        raise SystemExit("buildid: %s does not define QWARK_BUILD" % path)
    return int(m.group(1))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("--dev", type=int, choices=(0, 1), default=None,
                    help="write the sidecar for a test build (1) or a release build (0)")
    ap.add_argument("--out", default=None, help="the sidecar to write (with --dev)")
    ap.add_argument("--id", default=None,
                    help="the ID the module was compiled with, rather than the sources' now")
    args = ap.parse_args(argv)

    ident = int(args.id, 0) if args.id is not None else build_id()
    if args.out is None:
        print("0x%08X" % ident)
        return 0
    if args.dev is None:
        ap.error("--out needs --dev")

    text = ("# qwark.build: the qwark.sprx beside this file (tools/buildid.py)\n"
            "build %d\ndev %d\nid 0x%08X\n" % (qwark_build(), args.dev, ident))
    with open(args.out, "w", newline="\n") as f:
        f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
