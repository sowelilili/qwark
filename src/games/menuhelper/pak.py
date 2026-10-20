#!/usr/bin/env python3
"""
pak.py: writes ui.pak, the file on the console's HDD that carries the console
menu's renderers (build 55, PROTOCOL.md section 9.10), out of the committed
src/games/menuhelper_bins.c. qwark itself carries none of them any more: it
reads the running game's renderer from /dev_hdd0/qwark/ui.pak when it puts the
menu into the game.

From the qwark checkout:

    python -I src/games/menuhelper/pak.py --out ui.pak      the pak, for this build
    python -I src/games/menuhelper/pak.py --id              the renderers' identity
    python -I src/games/menuhelper/pak.py --show FILE       what a pak holds

`make` runs the first beside qwark.sprx, the host tests and smoke.py run it
into their fake HDDs, and build.sh runs the second into menuhelper_bins.h as
MH_PAK_ID: a module accepts only a pak that names its own build and carries
exactly the renderers it was built with. --build N stamps another build, for
the tests of a pak from another build.

The layout, every integer big-endian:

  header, 32 bytes
    0   magic "QPAK"
    4   u16 format, 1
    6   u16 header size, 32
    8   u32 qwark build (QWARK_BUILD)
    12  u32 CRC-32 of everything after the header: the renderers' identity
    16  u32 length of everything after the header
    20  u8 entries, then 3 bytes of 0, then 8 bytes of 0
  table of contents, 32 bytes an entry
    0   u8 game id, u8 kind (1 a renderer), u16 0
    4   char title[12], NUL-padded
    16  u32 the entry's offset in the file, u32 its length, u32 its CRC-32, u32 0
  an entry
    0   the descriptor, 40 bytes: u32 mailbox, mailbox size, state, mode word,
        its paused value, pending word, its none value, screen word, the
        menu's screen; u8 caves, u8 hook words, u8 Help row words, u8 0
    40  the caves, 20 bytes each: u32 address, u32 offset of its bytes in the
        entry, u32 length, u32 zeros after it, u8 1 for code, 3 bytes of 0
    ..  the hook words, 12 bytes each: u32 address, value, the game's word
    ..  the caves' bytes, in order
"""
import argparse
import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
QWARK = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))   # the qwark checkout
GEN_C = os.path.join(QWARK, "src", "games", "menuhelper_bins.c")
PROTO_H = os.path.join(QWARK, "src", "core", "proto.h")

MAGIC = b"QPAK"
FORMAT = 1
HEADER_SIZE = 32
TOC_SIZE = 32
DESC_SIZE = 40
CAVE_SIZE = 20
HOOK_SIZE = 12
KIND_RENDERER = 1
# What the module's loader has room for (src/core/uipak.h).
MAX_ENTRIES = 8
MAX_CAVES = 8
MAX_HOOKS = 32


def crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF


def qwark_build(path=PROTO_H):
    m = re.search(r"^#define\s+QWARK_BUILD\s+(\d+)", open(path, encoding="utf-8").read(), re.M)
    if not m:
        raise SystemExit("pak: %s does not define QWARK_BUILD" % path)
    return int(m.group(1))


def title_of(g):
    """The renderer's title id, out of its header (MH_TITLE)."""
    path = os.path.join(HERE, "mh_%s.h" % g)
    m = re.search(r'^#define\s+MH_TITLE\s+"([^"]*)"', open(path, encoding="utf-8").read(), re.M)
    return m.group(1) if m else ""


def parse_table(path=GEN_C):
    """
    Every renderer in menuhelper_bins.c, in mh_descs' order: a dict with the
    game, its short name, the descriptor's numbers, the caves (address, bytes,
    code, zeros) and the hook words (address, value, the game's word).
    """
    text = open(path, encoding="utf-8").read()

    def array(name):
        m = re.search(r"static const u8 %s\[\] = \{(.*?)\};" % re.escape(name), text, re.S)
        if not m:
            raise SystemExit("pak: %s has no array %s" % (path, name))
        return bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))

    m = re.search(r"static const struct mh_desc mh_descs\[\] = \{(.*?)\n\};", text, re.S)
    if not m:
        raise SystemExit("pak: %s has no mh_descs" % path)
    out = []
    num = r"\s*(0x[0-9A-Fa-f]+|\d+)u?\s*"
    pat = (r"\{\s*(\d+),\s*mh_(\w+)_caves,\s*(\d+),\s*mh_\w+_hooks,\s*(\d+),\s*(\d+)," +
           ",".join([num] * 9) + r"\}")
    for d in re.finditer(pat, m.group(1)):
        game, g = int(d.group(1)), d.group(2)
        ncaves, nhooks, nhelp = int(d.group(3)), int(d.group(4)), int(d.group(5))
        words = [int(x, 0) for x in d.groups()[5:14]]
        cm = re.search(r"static const struct mh_cave mh_%s_caves\[\] = \{(.*?)\};" % g, text, re.S)
        caves = []
        for c in re.finditer(r"\{ (0x[0-9A-Fa-f]+)u, (\w+), \(u32\)sizeof\(\w+\), (\d), (\d+)u \}", cm.group(1)):
            caves.append((int(c.group(1), 16), array(c.group(2)), int(c.group(3)), int(c.group(4))))
        hm = re.search(r"static const struct mh_word mh_%s_hooks\[\] = \{(.*?)\};" % g, text, re.S)
        hooks = []
        for w in re.finditer(r"\{ (0x[0-9A-Fa-f]+)u, (0x[0-9A-Fa-f]+)u, (0x[0-9A-Fa-f]+)u \}", hm.group(1)):
            hooks.append((int(w.group(1), 16), int(w.group(2), 16), int(w.group(3), 16)))
        if len(caves) != ncaves or len(hooks) != nhooks:
            raise SystemExit("pak: %s's table has %d caves and %d hooks, its descriptor says %d and %d" %
                             (g, len(caves), len(hooks), ncaves, nhooks))
        out.append({"game": game, "g": g, "title": title_of(g), "words": words,
                    "caves": caves, "hooks": hooks, "nhelp": nhelp})
    if not out:
        raise SystemExit("pak: no renderer in %s" % path)
    return out


def entry_bytes(r):
    caves, hooks = r["caves"], r["hooks"]
    if len(caves) > MAX_CAVES or len(hooks) > MAX_HOOKS or r["nhelp"] > len(hooks):
        raise SystemExit("pak: %s has %d caves and %d hooks, the loader takes %d and %d" %
                         (r["g"], len(caves), len(hooks), MAX_CAVES, MAX_HOOKS))
    head = struct.pack(">9I4B", *(r["words"] + [len(caves), len(hooks), r["nhelp"], 0]))
    at = DESC_SIZE + CAVE_SIZE * len(caves) + HOOK_SIZE * len(hooks)
    recs, blobs = b"", b""
    for addr, body, code, zeros in caves:
        recs += struct.pack(">4I4B", addr, at, len(body), zeros, code, 0, 0, 0)
        blobs += body
        at += len(body)
    for addr, value, orig in hooks:
        recs += struct.pack(">3I", addr, value, orig)
    return head + recs + blobs


def body_bytes(renderers):
    """Everything after the header: the table of contents and the entries."""
    if len(renderers) > MAX_ENTRIES:
        raise SystemExit("pak: %d renderers, the loader takes %d" % (len(renderers), MAX_ENTRIES))
    entries = [entry_bytes(r) for r in renderers]
    at = HEADER_SIZE + TOC_SIZE * len(entries)
    toc, data = b"", b""
    for r, e in zip(renderers, entries):
        title = r["title"].encode("ascii")[:12]
        toc += struct.pack(">BBH12s4I", r["game"], KIND_RENDERER, 0, title, at, len(e), crc32(e), 0)
        data += e
        at += len(e)
    return toc + data


def pak_bytes(renderers, build):
    body = body_bytes(renderers)
    head = struct.pack(">4sHHIIIB3x8x", MAGIC, FORMAT, HEADER_SIZE, build, crc32(body), len(body), len(renderers))
    return head + body


def parse_pak(b):
    """A pak's header and renderers, in parse_table's shape; raises ValueError on anything malformed."""
    if len(b) < HEADER_SIZE:
        raise ValueError("%d bytes, shorter than the header" % len(b))
    magic, fmt, hsize, build, crc, blen, n = struct.unpack(">4sHHIIIB", b[:21])
    if magic != MAGIC or fmt != FORMAT or hsize != HEADER_SIZE:
        raise ValueError("not a format %d pak" % FORMAT)
    if len(b) != HEADER_SIZE + blen:
        raise ValueError("%d bytes after the header, the header says %d" % (len(b) - HEADER_SIZE, blen))
    if crc32(b[HEADER_SIZE:]) != crc:
        raise ValueError("the CRC does not hold")
    out = []
    for i in range(n):
        game, kind, _, title, at, length, ecrc, _ = struct.unpack(
            ">BBH12s4I", b[HEADER_SIZE + TOC_SIZE * i:HEADER_SIZE + TOC_SIZE * (i + 1)])
        e = b[at:at + length]
        if kind != KIND_RENDERER or len(e) != length or crc32(e) != ecrc:
            raise ValueError("entry %d is not a whole renderer" % i)
        v = struct.unpack(">9I4B", e[:DESC_SIZE])
        ncaves, nhooks, nhelp = v[9], v[10], v[11]
        caves, hooks = [], []
        p = DESC_SIZE
        for _ in range(ncaves):
            addr, off, ln, zeros, code = struct.unpack(">4IB", e[p:p + 17])
            if off + ln > length:
                raise ValueError("entry %d: a cave runs past the entry" % i)
            caves.append((addr, e[off:off + ln], code, zeros))
            p += CAVE_SIZE
        for _ in range(nhooks):
            hooks.append(struct.unpack(">3I", e[p:p + HOOK_SIZE]))
            p += HOOK_SIZE
        out.append({"game": game, "title": title.rstrip(b"\0").decode("ascii"), "words": list(v[:9]),
                    "caves": caves, "hooks": hooks, "nhelp": nhelp})
    return build, crc, out


def main():
    ap = argparse.ArgumentParser(description="ui.pak, the console menu's renderers on the console's HDD")
    ap.add_argument("--out", help="write the pak here")
    ap.add_argument("--id", action="store_true", help="print the renderers' identity, the body's CRC-32")
    ap.add_argument("--build", type=int, default=None, help="stamp this build rather than QWARK_BUILD")
    ap.add_argument("--table", default=GEN_C, help="the generated menuhelper_bins.c")
    ap.add_argument("--show", help="print what a pak holds")
    args = ap.parse_args()

    if args.show:
        data = open(args.show, "rb").read()
        try:
            build, crc, rs = parse_pak(data)
        except ValueError as e:
            print("%s: %s" % (args.show, e))
            return 1
        print("%s: build %d, renderers 0x%08X, %d bytes" % (args.show, build, crc, len(data)))
        for r in rs:
            print("  game %d %s: %d caves, %d hook words (%d Help), %d bytes of caves" %
                  (r["game"], r["title"], len(r["caves"]), len(r["hooks"]), r["nhelp"],
                   sum(len(c[1]) for c in r["caves"])))
        return 0

    renderers = parse_table(args.table)
    if args.id:
        print("0x%08X" % crc32(body_bytes(renderers)))
        return 0
    if not args.out:
        ap.error("--out, --id or --show")
    build = qwark_build() if args.build is None else args.build
    data = pak_bytes(renderers, build)
    with open(args.out, "wb") as f:
        f.write(data)
    print("pak: %s, build %d, renderers 0x%08X, %d bytes" %
          (args.out, build, crc32(data[HEADER_SIZE:]), len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
