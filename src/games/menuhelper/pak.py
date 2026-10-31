#!/usr/bin/env python3
"""
pak.py: writes ui.pak, the file on the console's HDD that carries what qwark
writes into a game but has no need to hold itself (PROTOCOL.md section 9.10):
the console menu's renderers (build 55), out of the committed
src/games/menuhelper_bins.c, and the savefile helpers (build 56), out of the
committed src/games/sfhelper_bins.c. The module carries neither: it reads the
running game's part of /dev_hdd0/qwark/ui.pak when it puts the menu or the
helper into the game.

From the qwark checkout:

    python -I src/games/menuhelper/pak.py --out ui.pak      the pak, for this build
    python -I src/games/menuhelper/pak.py --id              the renderers' identity
    python -I src/games/menuhelper/pak.py --sf-id           the helpers' identity
    python -I src/games/menuhelper/pak.py --show FILE       what a pak holds
    python -I src/games/menuhelper/pak.py --out ui.pak --tod  the same with ToD's renderer
    python -I src/games/menuhelper/pak.py --tod-id          its identity

2026-10-31: Tools of Destruction's renderer, a prototype, is in the same
table (game 5) but only the builds that have ToD (QWARK_HAS_TOD: a test build,
the host builds) pack it, with --tod: an entry of kind 3 after the helpers,
outside the header's identities, so a release's pak is byte for byte the
four renderers' and the four helpers' as before. A module with ToD takes that
entry only when its CRC-32 is MH_TOD_PAK_ID, which build.sh writes into
menuhelper_bins.h with --tod-id. A module without ToD never looks at kind 3,
so it takes a test build's pak as readily as its own.

`make` runs the first beside qwark.sprx, the host tests and smoke.py run it
into their fake HDDs, menuhelper/build.sh runs the second into
menuhelper_bins.h as MH_PAK_ID, and sfhelper/build.sh the third into
sfhelper_bins.h as SF_PAK_ID: a module accepts only a pak that names its own
build and carries exactly the renderers and the helpers it was built with.
--build N stamps another build, for the tests of a pak from another build.

The layout, format 2 (build 56), every integer big-endian:

  header, 32 bytes
    0   magic "QPAK"
    4   u16 format, 2
    6   u16 header size, 32
    8   u32 qwark build (QWARK_BUILD)
    12  u32 CRC-32 of everything after the header
    16  u32 length of everything after the header
    20  u8 entries, then 3 bytes of 0
    24  u32 the renderers' identity: the CRC-32 of the kind 1 entries, one
        after another in the table's order (MH_PAK_ID)
    28  u32 the helpers' identity: the same of the kind 2 entries (SF_PAK_ID)
  table of contents, 32 bytes an entry, the renderers first
    0   u8 game id, u8 kind (1 a renderer, 2 a savefile helper), u16 0
    4   char title[12], NUL-padded
    16  u32 the entry's offset in the file, u32 its length, u32 its CRC-32, u32 0
  an entry
    0   the descriptor, 40 bytes: nine u32 words, then u8 caves, u8 hook
        words, u8 Help row words, u8 0. A renderer's words are its mailbox,
        mailbox size, state, mode word, its paused value, pending word, its
        none value, screen word and the menu's screen. A helper's are its
        three request bytes (api_mod, api_load, api_setaside), the aside
        buffer and its size, the trophy reset's request byte or 0, and three
        0s; it has no Help row words.
    40  the caves, 20 bytes each: u32 address, u32 offset of its bytes in the
        entry, u32 length, u32 zeros after it, u8 1 for code, 3 bytes of 0
        (a helper's caves are all code, with no zeros after them)
    ..  the hook words, 12 bytes each: u32 address, value, the game's word
        (0 for a helper's, which never takes its hook out again)
    ..  the caves' bytes, in order

Format 1 (build 55) had no kind 2 and carried the renderers' identity at 12,
where it was also the CRC of the rest; 24 and 28 were 0.
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
SF_GEN_C = os.path.join(QWARK, "src", "games", "sfhelper_bins.c")
PROTO_H = os.path.join(QWARK, "src", "core", "proto.h")

MAGIC = b"QPAK"
FORMAT = 2
HEADER_SIZE = 32
TOC_SIZE = 32
DESC_SIZE = 40
CAVE_SIZE = 20
HOOK_SIZE = 12
KIND_RENDERER = 1
KIND_SFHELPER = 2
KIND_TOD = 3            # 2026-10-31: Tools of Destruction's renderer, outside the identities
GAME_TOD = 5
# What the module's loader has room for (src/core/uipak.h).
MAX_ENTRIES = 16
MAX_CAVES = 8
MAX_HOOKS = 32
# A helper's entry is read whole into the tick's scratch (QSCRATCH_BYTES, util.h).
MAX_SF_ENTRY = 8192


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


def parse_table(path=GEN_C, tod=False):
    """
    Every renderer in menuhelper_bins.c, in mh_descs' order: a dict with the
    game, its short name, the descriptor's numbers, the caves (address, bytes,
    code, zeros) and the hook words (address, value, the game's word). The
    four originals' (kind 1) by default; with `tod`, Tools of Destruction's
    alone (kind 3).
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
        if (game == GAME_TOD) != tod:
            continue
        out.append({"game": game, "g": g, "title": title_of(g), "words": words,
                    "caves": caves, "hooks": hooks, "nhelp": nhelp,
                    "kind": KIND_TOD if tod else KIND_RENDERER})
    if not out:
        raise SystemExit("pak: no %srenderer in %s" % ("ToD " if tod else "", path))
    return out


def parse_sf_table(path=SF_GEN_C):
    """
    Every savefile helper in sfhelper_bins.c, in sf_descs' order, in parse_table's
    shape: the six numbers the descriptor carries and three 0s for its words, the
    caves (address, bytes, 1 for code, no zeros) and the hook words (address,
    value, 0). The arrays sit behind QWARK_EMULATOR in that file; this reads the
    text, which has them whatever a compiler would keep.
    """
    text = open(path, encoding="utf-8").read()

    def array(name):
        m = re.search(r"static const u8 %s\[\] = \{(.*?)\};" % re.escape(name), text, re.S)
        if not m:
            raise SystemExit("pak: %s has no array %s" % (path, name))
        return bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))

    m = re.search(r"static const struct sf_desc sf_descs\[\] = \{(.*?)\n\};", text, re.S)
    if not m:
        raise SystemExit("pak: %s has no sf_descs" % path)
    out = []
    num = r"\s*(0x[0-9A-Fa-f]+|\d+)u\s*"
    pat = (r"\{\s*GAME_RAC(\d),\s*SF_TABLE\(sf_(\w+)_caves\),\s*(\d+),\s*SF_TABLE\(sf_\w+_hooks\),\s*(\d+)," +
           ",".join([num] * 6) + r"\}")
    for d in re.finditer(pat, m.group(1)):
        game, g = int(d.group(1)), d.group(2)
        ncaves, nhooks = int(d.group(3)), int(d.group(4))
        words = [int(x, 0) for x in d.groups()[4:10]] + [0, 0, 0]
        cm = re.search(r"static const struct sf_cave sf_%s_caves\[\] = \{(.*?)\};" % g, text, re.S)
        hm = re.search(r"static const struct patch_word sf_%s_hooks\[\] = \{(.*?)\};" % g, text, re.S)
        if not cm or not hm:
            raise SystemExit("pak: %s has no caves or hook words for %s" % (path, g))
        caves = [(int(c.group(1), 16), array(c.group(2)), 1, 0)
                 for c in re.finditer(r"\{ (0x[0-9A-Fa-f]+)u, (\w+), \(u32\)sizeof\(\w+\) \}", cm.group(1))]
        hooks = [(int(w.group(1), 16), int(w.group(2), 16), 0)
                 for w in re.finditer(r"\{ (0x[0-9A-Fa-f]+)u, (0x[0-9A-Fa-f]+)u \}", hm.group(1))]
        if len(caves) != ncaves or len(hooks) != nhooks:
            raise SystemExit("pak: %s's helper has %d caves and %d hooks, its descriptor says %d and %d" %
                             (g, len(caves), len(hooks), ncaves, nhooks))
        out.append({"game": game, "g": g, "title": title_of(g), "words": words,
                    "caves": caves, "hooks": hooks, "nhelp": 0, "kind": KIND_SFHELPER})
    if not out:
        raise SystemExit("pak: no savefile helper in %s" % path)
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
    e = head + recs + blobs
    if r.get("kind", KIND_RENDERER) == KIND_SFHELPER and len(e) > MAX_SF_ENTRY:
        raise SystemExit("pak: %s's helper is %d bytes, the module reads at most %d" % (r["g"], len(e), MAX_SF_ENTRY))
    return e


def identity(parts):
    """A kind's identity: the CRC-32 of its entries, one after another."""
    return crc32(b"".join(entry_bytes(r) for r in parts))


def body_bytes(renderers, helpers, extra=()):
    """
    Everything after the header: the table of contents and the entries, the
    renderers first, then the helpers, then any ToD renderer (kind 3).
    """
    parts = list(renderers) + list(helpers) + list(extra)
    if len(parts) > MAX_ENTRIES:
        raise SystemExit("pak: %d entries, the loader takes %d" % (len(parts), MAX_ENTRIES))
    entries = [entry_bytes(r) for r in parts]
    at = HEADER_SIZE + TOC_SIZE * len(entries)
    toc, data = b"", b""
    for r, e in zip(parts, entries):
        title = r["title"].encode("ascii")[:12]
        toc += struct.pack(">BBH12s4I", r["game"], r.get("kind", KIND_RENDERER), 0, title, at, len(e), crc32(e), 0)
        data += e
        at += len(e)
    return toc + data


def pak_bytes(renderers, helpers, build, extra=()):
    body = body_bytes(renderers, helpers, extra)
    head = struct.pack(">4sHHIIIB3xII", MAGIC, FORMAT, HEADER_SIZE, build, crc32(body), len(body),
                       len(renderers) + len(helpers) + len(extra), identity(renderers), identity(helpers))
    return head + body


def parse_pak(b):
    """
    A pak's header and entries: (build, renderers' id, helpers' id, [renderer],
    [helper]), each entry in parse_table's shape; raises ValueError on anything
    malformed.
    """
    if len(b) < HEADER_SIZE:
        raise ValueError("%d bytes, shorter than the header" % len(b))
    magic, fmt, hsize, build, crc, blen, n, mh_id, sf_id = struct.unpack(">4sHHIIIB3xII", b[:HEADER_SIZE])
    if magic != MAGIC or hsize != HEADER_SIZE:
        raise ValueError("not a pak")
    if fmt != FORMAT:
        raise ValueError("a format %d pak, not format %d" % (fmt, FORMAT))
    if len(b) != HEADER_SIZE + blen:
        raise ValueError("%d bytes after the header, the header says %d" % (len(b) - HEADER_SIZE, blen))
    if crc32(b[HEADER_SIZE:]) != crc:
        raise ValueError("the CRC does not hold")
    out = {KIND_RENDERER: [], KIND_SFHELPER: [], KIND_TOD: []}
    raw = {KIND_RENDERER: b"", KIND_SFHELPER: b"", KIND_TOD: b""}
    for i in range(n):
        game, kind, _, title, at, length, ecrc, _ = struct.unpack(
            ">BBH12s4I", b[HEADER_SIZE + TOC_SIZE * i:HEADER_SIZE + TOC_SIZE * (i + 1)])
        e = b[at:at + length]
        if kind not in out or len(e) != length or crc32(e) != ecrc:
            raise ValueError("entry %d is not a whole renderer or helper" % i)
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
        out[kind].append({"game": game, "title": title.rstrip(b"\0").decode("ascii"), "words": list(v[:9]),
                          "caves": caves, "hooks": hooks, "nhelp": nhelp, "kind": kind})
        raw[kind] += e
    if crc32(raw[KIND_RENDERER]) != mh_id or crc32(raw[KIND_SFHELPER]) != sf_id:
        raise ValueError("the identities at 24 and 28 are not the entries'")
    return build, mh_id, sf_id, out[KIND_RENDERER], out[KIND_SFHELPER] + out[KIND_TOD]


def main():
    ap = argparse.ArgumentParser(description="ui.pak, the renderers and savefile helpers on the console's HDD")
    ap.add_argument("--out", help="write the pak here")
    ap.add_argument("--id", action="store_true", help="print the renderers' identity (MH_PAK_ID)")
    ap.add_argument("--sf-id", action="store_true", help="print the savefile helpers' identity (SF_PAK_ID)")
    ap.add_argument("--build", type=int, default=None, help="stamp this build rather than QWARK_BUILD")
    ap.add_argument("--table", default=GEN_C, help="the generated menuhelper_bins.c")
    ap.add_argument("--sf-table", default=SF_GEN_C, help="the generated sfhelper_bins.c")
    ap.add_argument("--show", help="print what a pak holds")
    ap.add_argument("--tod", action="store_true",
                    help="pack Tools of Destruction's renderer too (kind 3), as the builds with ToD do")
    ap.add_argument("--tod-id", action="store_true", help="print ToD's renderer's identity (MH_TOD_PAK_ID)")
    args = ap.parse_args()

    if args.show:
        data = open(args.show, "rb").read()
        try:
            build, mh_id, sf_id, rs, hs = parse_pak(data)
        except ValueError as e:
            print("%s: %s" % (args.show, e))
            return 1
        print("%s: build %d, renderers 0x%08X, helpers 0x%08X, %d bytes" % (args.show, build, mh_id, sf_id, len(data)))
        for r in rs:
            print("  game %d %s: renderer, %d caves, %d hook words (%d Help), %d bytes of caves" %
                  (r["game"], r["title"], len(r["caves"]), len(r["hooks"]), r["nhelp"],
                   sum(len(c[1]) for c in r["caves"])))
        for r in hs:
            print("  game %d: %s, %d caves, %d hook words, %d bytes of caves" %
                  (r["game"], "ToD renderer" if r["kind"] == KIND_TOD else "savefile helper",
                   len(r["caves"]), len(r["hooks"]), sum(len(c[1]) for c in r["caves"])))
        return 0

    if args.id:
        print("0x%08X" % identity(parse_table(args.table)))
        return 0
    if args.sf_id:
        print("0x%08X" % identity(parse_sf_table(args.sf_table)))
        return 0
    if args.tod_id:
        print("0x%08X" % identity(parse_table(args.table, tod=True)))
        return 0
    if not args.out:
        ap.error("--out, --id, --sf-id or --show")
    renderers = parse_table(args.table)
    helpers = parse_sf_table(args.sf_table)
    build = qwark_build() if args.build is None else args.build
    extra = parse_table(args.table, tod=True) if args.tod else []
    data = pak_bytes(renderers, helpers, build, extra)
    with open(args.out, "wb") as f:
        f.write(data)
    print("pak: %s, build %d, renderers 0x%08X, helpers 0x%08X%s, %d bytes" %
          (args.out, build, identity(renderers), identity(helpers),
           ", ToD 0x%08X" % identity(extra) if extra else "", len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
