#!/usr/bin/env python3
"""
pkg.py: qwark-installer.pkg, around the compiled EBOOT.BIN: its PARAM.SFO and
ICON0.PNG, the package itself, and the check of the finished package. `make
pkg` runs it. It needs the Python standard library, and for the check the SDK's
own make_package_npdrm and make_fself_npdrm and qwark's scetool.

From the qwark checkout:

    python -I installer/pkg.py content-id
        the package's content id, which the makefile signs EBOOT.BIN with

    python -I installer/pkg.py stage --sprx qwark.sprx --pak ui.pak --out installer/build
        installer/build/pkg/ holds PARAM.SFO, ICON0.PNG and USRDIR/ with the
        module and the pak; EBOOT.BIN goes into USRDIR/ after this

    python -I installer/pkg.py build --root installer/build/pkg --out qwark-installer.pkg
        the package of that folder

    python -I installer/pkg.py check --pkg qwark-installer.pkg --sprx qwark.sprx
                                     --pak ui.pak --elf installer/build/EBOOT.elf
        reads the package back and holds every file in it against what went in;
        has the SDK's make_package_npdrm read, check and list it; decrypts
        EBOOT.BIN with scetool and holds its segments against the ELF that was
        signed; and has the SDK pack the same files itself, then packs them
        again here and holds the two packages to each other, byte for byte.
        It never asks the SDK to unpack it: make_package_npdrm --extract never
        finishes an EBOOT.BIN that scetool signed, and writes for ever

    python -I installer/pkg.py icon OUT.png
        ICON0.PNG on its own, to look at

Why a packager of our own: the SDK's make_package_npdrm packs a game for
Sony's own signing, so it takes only an EBOOT.BIN that make_fself_npdrm made,
which it then signs as a debug NPDRM program, and no SELF that is not an NPDRM
one. A debug program does not run on PS3HEN, and qwark.sprx is not an NPDRM
SELF. So EBOOT.BIN is signed by scetool as homebrew is, the way webMAN MOD's
installer is, and this script packs it. The package is the SDK's own debug
format (below), the format webMAN MOD's packages are in, which Package Manager
installs on CFW and on PS3HEN alike; the check makes the SDK's packager produce
a package of the same files and holds this script to it, and has the SDK's own
reader read the result.

The title id, QWRK00001, is no real title's: real ones start with a
four-letter Sony prefix (BLES, NPEA, ...), and homebrew takes a name of its
own, as webMAN's UPDWEBMOD does. install.h has the same id.

A debug package, every integer big-endian:

  header, 0xC0 bytes
    0x00  7F "PKG", u16 revision 0 (debug), u16 type 1 (PS3)
    0x08  u32 metadata offset (0xC0), u32 its entries, u32 its size
    0x14  u32 items, u64 package size, u64 data offset, u64 data size
    0x30  the content id, NUL-padded to 0x30
    0x60  16 bytes, the key for the data
    0x70  16 bytes the console does not use for a debug package
    0x80  a digest block of the 0x80 bytes before it
  metadata: entries of u32 id, u32 size, bytes; then a digest block of them
  data, encrypted: 0x20 bytes an item (u32 name offset, u32 name length,
    u64 data offset, u64 data length, u32 flags, u32 0), each name padded to
    16 bytes, then each file's bytes padded to 16; offsets from the data's start
  footer: a digest block of everything before it, the SHA-1 of everything
    before the last 0x20 bytes, and 12 bytes of 0

  A digest block is 0x40 bytes: bytes 3 to 19 of the SHA-1 of what it covers,
  then 0x30 bytes of that seed's keystream. The keystream for a 16-byte seed
  is, block n, the first 16 bytes of the SHA-1 of: seed[0:8] twice,
  seed[8:16] twice, 24 bytes of 0 and n as a u64; the data is XORed with the
  keystream of the key at 0x60.
"""
import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
QWARK = os.path.dirname(HERE)
PROTO_H = os.path.join(QWARK, "src", "core", "proto.h")
INSTALL_H = os.path.join(HERE, "install.h")

TITLE_ID = "QWRK00001"
CONTENT_ID = "UP0001-%s_00-QWARKINSTALLER00" % TITLE_ID

# What PARAM.SFO asks of the console. 4.70 is the SDK the app is built with;
# PS3HEN starts at 4.80 and every CFW qwark runs on is past it.
PS3_SYSTEM_VER = "04.7000"

# The files the package installs, and nothing else.
PKG_FILES = ["ICON0.PNG", "PARAM.SFO", "USRDIR/EBOOT.BIN", "USRDIR/qwark.sprx", "USRDIR/ui.pak"]

# Items, in the SDK packager's order: PARAM.SFO and ICON0.PNG, the folder, EBOOT.BIN first in it.
ITEM_ORDER = ["PARAM.SFO", "ICON0.PNG", "USRDIR", "USRDIR/EBOOT.BIN", "USRDIR/qwark.sprx", "USRDIR/ui.pak"]

# Item flags: 0x80000000 overwrite, then the kind.
ITEM_SELF = 0x80000001      # an NPDRM SELF: EBOOT.BIN
ITEM_RAW = 0x80000003       # a file as it is
ITEM_DIR = 0x80000004       # a folder

# What make_package_npdrm revision 1972, this SDK's, writes for a free GameExec package.
PACKAGER_REVISION = 0x1972
PACKAGE_VERSION = "01.00"   # the package format's; a package of any build installs over any other


def die(msg):
    sys.stderr.write("pkg.py: %s\n" % msg)
    sys.exit(1)


def qwark_build(path=PROTO_H):
    m = re.search(r"^#define\s+QWARK_BUILD\s+(\d+)", open(path, encoding="utf-8").read(), re.M)
    if not m:
        die("%s does not define QWARK_BUILD" % path)
    return int(m.group(1))


def app_ver(build):
    """The build as PARAM.SFO's NN.NN: build 55 is 00.55, build 123 would be 01.23."""
    if not 0 <= build <= 9999:
        die("build %d does not fit APP_VER" % build)
    return "%02d.%02d" % divmod(build, 100)


def title(build):
    return "qwark installer (build %d)" % build


def check_install_h():
    """install.h finds the package's files by this title id, so the two must agree."""
    m = re.search(r'^#define\s+INST_TITLE_ID\s+"([^"]+)"', open(INSTALL_H, encoding="utf-8").read(), re.M)
    if not m or m.group(1) != TITLE_ID:
        die("install.h's INST_TITLE_ID is not %s" % TITLE_ID)


# ---------------------------------------------------------------- PARAM.SFO

FMT_UTF8 = 0x0204
FMT_INT32 = 0x0404


def sfo_entries(build):
    """(key, format, value, max length). The console wants the keys in order."""
    return sorted([
        ("APP_VER", FMT_UTF8, app_ver(build), 8),
        ("BOOTABLE", FMT_INT32, 1, 4),
        ("CATEGORY", FMT_UTF8, "HG", 4),
        ("LICENSE", FMT_UTF8, "qwark is free software under the GNU GPL v3. "
                              "It is not made, approved or supported by Sony.", 512),
        ("PARENTAL_LEVEL", FMT_INT32, 0, 4),
        ("PS3_SYSTEM_VER", FMT_UTF8, PS3_SYSTEM_VER, 8),
        ("RESOLUTION", FMT_INT32, 63, 4),
        ("SOUND_FORMAT", FMT_INT32, 1, 4),
        ("TITLE", FMT_UTF8, title(build), 128),
        ("TITLE_ID", FMT_UTF8, TITLE_ID, 16),
        ("VERSION", FMT_UTF8, "01.00", 8),
    ])


def make_sfo(build):
    """
    PARAM.SFO, little-endian: a 20-byte header ("\\0PSF", version 1.1, the key
    table's offset, the data table's, the count), 16 bytes an entry (key offset,
    format, length, max length, data offset), the NUL-terminated keys padded to
    four bytes, then each value padded to its max length.
    """
    entries = sfo_entries(build)
    keys = b""
    data = b""
    index = b""
    for key, fmt, value, maxlen in entries:
        raw = (value.encode("utf-8") + b"\0") if fmt == FMT_UTF8 else struct.pack("<I", value)
        if len(raw) > maxlen:
            die("PARAM.SFO %s is %d bytes, more than %d" % (key, len(raw), maxlen))
        index += struct.pack("<HHIII", len(keys), fmt, len(raw), maxlen, len(data))
        keys += key.encode("ascii") + b"\0"
        data += raw + b"\0" * (maxlen - len(raw))
    keys += b"\0" * (-len(keys) % 4)
    key_start = 20 + len(index)
    data_start = key_start + len(keys)
    return struct.pack("<4sIIII", b"\0PSF", 0x0101, key_start, data_start, len(entries)) + index + keys + data


def read_sfo(blob):
    if len(blob) < 20:
        die("PARAM.SFO is cut short")
    magic, _version, key_start, data_start, count = struct.unpack_from("<4sIIII", blob, 0)
    if magic != b"\0PSF":
        die("PARAM.SFO has no PSF magic")
    out = {}
    for i in range(count):
        koff, fmt, length, _maxlen, doff = struct.unpack_from("<HHIII", blob, 20 + 16 * i)
        key = blob[key_start + koff:blob.index(b"\0", key_start + koff)].decode("ascii")
        raw = blob[data_start + doff:data_start + doff + length]
        out[key] = struct.unpack("<I", raw)[0] if fmt == FMT_INT32 else raw.rstrip(b"\0").decode("utf-8")
    return out


# ---------------------------------------------------------------- ICON0.PNG

# Drawn here, from these few letters, so the package carries no art of anyone's.
WORD = {  # 5 wide, 9 tall: two rows of ascender, five of x-height, two of descender
    "q": [".....", ".....", ".####", "#...#", "#...#", ".####", "....#", "....#", "....#"],
    "w": [".....", ".....", "#...#", "#...#", "#.#.#", "#.#.#", ".#.#.", ".....", "....."],
    "a": [".....", ".....", ".###.", "....#", ".####", "#...#", ".####", ".....", "....."],
    "r": [".....", ".....", "#.##.", "##..#", "#....", "#....", "#....", ".....", "....."],
    "k": ["#....", "#....", "#..#.", "#.#..", "##...", "#.#..", "#..#.", ".....", "....."],
}
CAPS = {  # 5 wide, 7 tall
    "I": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####"],
    "N": ["#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"],
    "S": [".####", "#....", "#....", ".###.", "....#", "....#", "####."],
    "T": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    "A": [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "L": ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
    "E": ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
    "R": ["####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"],
}

ICON_W, ICON_H = 320, 176
TOP, BOTTOM = (0x24, 0x33, 0x58), (0x10, 0x17, 0x2A)
WORD_COLOUR = (0xFF, 0xC4, 0x3A)
CAPS_COLOUR = (0xC8, 0xD2, 0xE8)


def draw_text(px, text, font, scale, gap, x0, y0, colour):
    for n, ch in enumerate(text):
        for gy, row in enumerate(font[ch]):
            for gx, bit in enumerate(row):
                if bit != "#":
                    continue
                for dy in range(scale):
                    for dx in range(scale):
                        x = x0 + n * (5 * scale + gap) + gx * scale + dx
                        y = y0 + gy * scale + dy
                        if 0 <= x < ICON_W and 0 <= y < ICON_H:
                            px[y][x] = colour


def text_width(text, scale, gap):
    return len(text) * 5 * scale + (len(text) - 1) * gap


def make_icon():
    """320 x 176, 8-bit RGB: a dark blue fade, "qwark" in yellow, a rule, INSTALLER under it."""
    px = []
    for y in range(ICON_H):
        t = y / (ICON_H - 1)
        c = tuple(int(round(a + (b - a) * t)) for a, b in zip(TOP, BOTTOM))
        px.append([c] * ICON_W)

    # The word with its descender, a rule under it, the caps under that: 104 rows, centred.
    scale, gap = 7, 7
    caps_scale, caps_gap = 3, 3
    top = (ICON_H - (9 * scale + 6 + 2 + 12 + 7 * caps_scale)) // 2
    draw_text(px, "qwark", WORD, scale, gap, (ICON_W - text_width("qwark", scale, gap)) // 2, top, WORD_COLOUR)
    rule_y = top + 9 * scale + 6
    for x in range(56, ICON_W - 56):
        px[rule_y][x] = px[rule_y + 1][x] = WORD_COLOUR
    draw_text(px, "INSTALLER", CAPS, caps_scale, caps_gap,
              (ICON_W - text_width("INSTALLER", caps_scale, caps_gap)) // 2, rule_y + 2 + 12, CAPS_COLOUR)

    raw = b"".join(b"\0" + bytes(v for p in row for v in p) for row in px)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", ICON_W, ICON_H, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


# ---------------------------------------------------------------- the package format

PKG_MAGIC = 0x7F504B47
HEADER_SIZE = 0xC0
FOOTER_SIZE = 0x60


def keystream_xor(seed, buf):
    """buf XORed with the keystream of a 16-byte seed (see the top of this file)."""
    key = bytearray(seed[0:8] * 2 + seed[8:16] * 2 + b"\0" * 0x20)
    out = bytearray(len(buf))
    for block in range((len(buf) + 15) // 16):
        struct.pack_into(">Q", key, 0x38, block)
        pad = hashlib.sha1(key).digest()
        lo = block * 16
        for i in range(lo, min(lo + 16, len(buf))):
            out[i] = buf[i] ^ pad[i - lo]
    return bytes(out)


def digest_block(covered):
    seed = hashlib.sha1(covered).digest()[3:19]
    return seed + keystream_xor(seed, b"\0" * 0x30)


def pad16(blob):
    return blob + b"\0" * (-len(blob) % 16)


def bcd(text):
    """"04.7000" -> 04 70 00: the digits two to a byte."""
    digits = text.replace(".", "")
    return bytes(int(digits[i:i + 2], 16) for i in range(0, len(digits), 2))


def pack_items(items):
    """The data before encryption: the item table, the names, the files."""
    names = b""
    name_offsets = []
    table_size = 0x20 * len(items)
    for name, _flags, _blob in items:
        name_offsets.append(table_size + len(names))
        names += pad16(name.encode("ascii"))
    cursor = table_size + len(names)
    table = b""
    files = b""
    for (name, flags, blob), name_off in zip(items, name_offsets):
        size = 0 if blob is None else len(blob)
        table += struct.pack(">IIQQII", name_off, len(name), cursor, size, flags, 0)
        if blob is not None:
            files += pad16(blob)
            cursor += len(pad16(blob))
    return table + names + files


def build_pkg(items, content_id, key, unused, meta):
    """
    A debug package of items, [(name, flags, bytes or None for a folder)], in
    that order, with the data key key, the 16 bytes at 0x70, and the metadata
    entries [(id, bytes)].
    """
    plain = pack_items(items)
    data = keystream_xor(key, plain)
    entries = b"".join(struct.pack(">II", mid, len(value)) + value for mid, value in meta)
    meta_region = entries + digest_block(entries)
    data_off = HEADER_SIZE + len(meta_region)
    total = data_off + len(data) + FOOTER_SIZE
    head = struct.pack(">IHHIIIIQQQ", PKG_MAGIC, 0, 1, HEADER_SIZE, len(meta), len(meta_region), len(items),
                       total, data_off, len(data))
    head += content_id.encode("ascii").ljust(0x30, b"\0") + key + unused
    head += digest_block(head)
    body = head + meta_region + data
    tail = digest_block(body)
    whole = hashlib.sha1(body + tail).digest()
    return body + tail + whole + b"\0" * (FOOTER_SIZE - 0x40 - len(whole))


def read_pkg(blob):
    """The parts of a debug package: its header fields, metadata entries and items."""
    if len(blob) < HEADER_SIZE + FOOTER_SIZE:
        die("not a package: too short")
    magic, revision, ptype, meta_off, meta_count, meta_size, count, total, data_off, data_size = \
        struct.unpack_from(">IHHIIIIQQQ", blob, 0)
    if magic != PKG_MAGIC:
        die("not a package: no PKG magic")
    if revision != 0 or ptype != 1:
        die("not a PS3 debug package (revision %04x, type %04x)" % (revision, ptype))
    meta = []
    o = meta_off
    for _ in range(meta_count):
        mid, size = struct.unpack_from(">II", blob, o)
        meta.append((mid, blob[o + 8:o + 8 + size]))
        o += 8 + size
    key = blob[0x60:0x70]
    plain = keystream_xor(key, blob[data_off:data_off + data_size])
    items = []
    for i in range(count):
        name_off, name_len, off, size, flags, _ = struct.unpack_from(">IIQQII", plain, 0x20 * i)
        name = plain[name_off:name_off + name_len].decode("ascii", "replace")
        items.append((name, flags, None if flags & 0xFF == 4 else plain[off:off + size]))
    return {
        "content_id": blob[0x30:0x60].rstrip(b"\0").decode("ascii"),
        "key": key, "unused": blob[0x70:0x80], "meta": meta, "items": items,
        "total": total, "size": len(blob), "meta_end": o, "meta_region_end": meta_off + meta_size,
        "digests_ok": (blob[0x80:0xC0] == digest_block(blob[:0x80])
                       and blob[o:meta_off + meta_size] == digest_block(blob[meta_off:o])
                       and blob[data_off + data_size:data_off + data_size + 0x40] == digest_block(blob[:data_off + data_size])
                       and blob[-0x20:-0x0C] == hashlib.sha1(blob[:-0x20]).digest()),
    }


def our_metadata(data_size, key, sfo):
    """Metadata as make_package_npdrm revision 1972 writes it for a free GameExec package."""
    return [
        (1, struct.pack(">I", 3)),          # DRM type: free
        (2, struct.pack(">I", 5)),          # content type: GameExec
        (3, struct.pack(">I", 0x4E)),       # package flags, as the SDK sets them
        (4, struct.pack(">Q", data_size)),
        (5, struct.pack(">H", PACKAGER_REVISION) + bcd(PACKAGE_VERSION)),
        (7, b"\0" * 8 + key),               # the QA digest; here the data key, which is ours to pick
        (8, b"\x81" + bcd(sfo["PS3_SYSTEM_VER"]) + bcd(sfo["VERSION"]) + bcd(sfo["APP_VER"])),
        (9, b"\0" * 8),
    ]


def items_of(root):
    items = []
    for name in ITEM_ORDER:
        path = os.path.join(root, *name.split("/"))
        if name == "USRDIR":
            if not os.path.isdir(path):
                die("%s has no USRDIR" % root)
            items.append((name, ITEM_DIR, None))
            continue
        if not os.path.isfile(path):
            die("%s has no %s" % (root, name))
        items.append((name, ITEM_SELF if name == "USRDIR/EBOOT.BIN" else ITEM_RAW, open(path, "rb").read()))
    extra = []
    for dirpath, _dirs, files in os.walk(root):
        for f in files:
            rel = os.path.relpath(os.path.join(dirpath, f), root).replace(os.sep, "/")
            if rel not in ITEM_ORDER:
                extra.append(rel)
    if extra:
        die("%s holds files the package does not carry: %s" % (root, ", ".join(sorted(extra))))
    return items


def cmd_build(args):
    check_install_h()
    items = items_of(args.root)
    sfo = read_sfo(dict((n, b) for n, _f, b in items)["PARAM.SFO"])
    plain = pack_items(items)
    # The data key: the SDK derives its own; any 16 bytes do, so these are the content's own SHA-1.
    key = hashlib.sha1(plain).digest()[:16]
    unused = hashlib.sha1(CONTENT_ID.encode("ascii") + key).digest()[:16]
    meta = our_metadata(len(plain), key, sfo)
    blob = build_pkg(items, CONTENT_ID, key, unused, meta)
    with open(args.out, "wb") as f:
        f.write(blob)
    print("pkg.py: %s, %d bytes: %s" % (args.out, len(blob), ", ".join(n for n, _f, _b in items)))


# ---------------------------------------------------------------- stage

def check_pak(blob, build, what):
    if len(blob) < 32 or blob[:4] != b"QPAK":
        die("%s is not a ui.pak" % what)
    pak_build = struct.unpack(">I", blob[8:12])[0]
    if pak_build != build:
        die("%s is build %d's; this package is build %d's" % (what, pak_build, build))


def check_sprx(blob, what):
    if blob[:4] != b"SCE\0":
        die("%s is not a signed module" % what)


def cmd_stage(args):
    check_install_h()
    if shutil.disk_usage(HERE).free < FREE_MIN:
        die("less than %d MB free" % (FREE_MIN >> 20))
    build = qwark_build()
    check_sprx(open(args.sprx, "rb").read(), args.sprx)
    check_pak(open(args.pak, "rb").read(), build, args.pak)

    root = os.path.join(args.out, "pkg")
    if os.path.isdir(root):
        shutil.rmtree(root)
    os.makedirs(os.path.join(root, "USRDIR"))
    with open(os.path.join(root, "PARAM.SFO"), "wb") as f:
        f.write(make_sfo(build))
    with open(os.path.join(root, "ICON0.PNG"), "wb") as f:
        f.write(make_icon())
    shutil.copyfile(args.sprx, os.path.join(root, "USRDIR", "qwark.sprx"))
    shutil.copyfile(args.pak, os.path.join(root, "USRDIR", "ui.pak"))
    print("pkg.py: staged %s for build %d (%s)" % (root, build, app_ver(build)))


# ---------------------------------------------------------------- check

def sdk_tool(name, sdk_bin):
    base = sdk_bin or os.path.join(os.environ.get("CELL_SDK", r"C:\usr\local\cell"), "host-win32", "bin")
    path = os.path.join(base, name + ".exe")
    if not os.path.isfile(path):
        die("no %s at %s; set CELL_SDK or pass --sdk-bin" % (name, path))
    return path


# The bounds every tool the check runs is held to. make_package_npdrm --extract
# never finishes an EBOOT.BIN that scetool signed and writes it out for ever (it
# filled a disk once), so the check never asks it to unpack anything, and
# whatever it does ask, of any tool, is stopped at the first of these.
TOOL_SECONDS = 120              # how long a tool may run
TOOL_OUTPUT_MAX = 1 << 20       # how much it may print
WORK_MAX = 64 << 20             # how much may pile up in the check's folder, where the tools write
FREE_MIN = 1 << 30              # the free space the check wants before it starts

CHECK_DIR = os.path.join(HERE, "build", "check")


def tree_size(path):
    total = 0
    for dirpath, _dirs, files in os.walk(path):
        for name in files:
            try:
                total += os.path.getsize(os.path.join(dirpath, name))
            except OSError:
                pass
    return total


def run(argv, cwd=None, ok_codes=(0,)):
    """A tool, with no input, its output in CHECK_DIR, and stopped at any of the bounds above."""
    os.makedirs(CHECK_DIR, exist_ok=True)
    log_path = os.path.join(CHECK_DIR, "tool.log")
    name = os.path.basename(argv[0])
    with open(log_path, "wb") as log:
        p = subprocess.Popen(argv, cwd=cwd, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
        start = time.monotonic()
        why = None
        while p.poll() is None:
            time.sleep(0.05)
            if time.monotonic() - start > TOOL_SECONDS:
                why = "ran for more than %d seconds" % TOOL_SECONDS
            elif os.path.getsize(log_path) > TOOL_OUTPUT_MAX:
                why = "printed more than %d bytes" % TOOL_OUTPUT_MAX
            elif tree_size(CHECK_DIR) > WORK_MAX:
                why = "wrote more than %d bytes" % WORK_MAX
            if why:
                p.kill()
                p.wait()
                break
    out = open(log_path, "rb").read(TOOL_OUTPUT_MAX).decode("utf-8", "replace")
    os.remove(log_path)
    if why:
        shutil.rmtree(CHECK_DIR, ignore_errors=True)
        die("%s %s, so it was stopped: %s" % (name, why, " ".join(argv[1:])))
    if p.returncode not in ok_codes:
        die("%s failed (%d):\n%s" % (name, p.returncode, out))
    return out


def load_segments(blob):
    """The PT_LOAD segments of a big-endian ELF64, as (offset, bytes)."""
    if blob[:4] != b"\x7fELF" or blob[4] != 2 or blob[5] != 2:
        die("not a big-endian ELF64")
    phoff = struct.unpack_from(">Q", blob, 0x20)[0]
    phentsize, phnum = struct.unpack_from(">HH", blob, 0x36)
    segs = []
    for i in range(phnum):
        ptype, _flags, off, _va, _pa, filesz = struct.unpack_from(">IIQQQQ", blob, phoff + i * phentsize)
        if ptype == 1:
            segs.append((off, blob[off:off + filesz]))
    return segs


# What make_package_npdrm --check says of any homebrew package: it would sign
# EBOOT.BIN itself, and it takes no SELF that is not an NPDRM one.
EXPECTED_COMPLAINTS = [
    ">> !ERROR!: USRDIR/EBOOT.BIN is not a supported NPDRM SELF. (already authorized)",
    ">> !ERROR!: USRDIR/qwark.sprx is not an NPDRM SELF.",
]


def check_contents(problems, files, args, build):
    """files: name -> bytes, from whichever reader unpacked the package."""
    if sorted(files) != sorted(PKG_FILES):
        problems.append("the package holds %s, not %s" % (sorted(files), sorted(PKG_FILES)))
        return
    if files["USRDIR/qwark.sprx"] != open(args.sprx, "rb").read():
        problems.append("USRDIR/qwark.sprx is not %s" % args.sprx)
    if files["USRDIR/ui.pak"] != open(args.pak, "rb").read():
        problems.append("USRDIR/ui.pak is not %s" % args.pak)
    check_pak(files["USRDIR/ui.pak"], build, "USRDIR/ui.pak")
    if files["ICON0.PNG"] != make_icon():
        problems.append("ICON0.PNG is not the one pkg.py draws")
    if files["PARAM.SFO"] != make_sfo(build):
        problems.append("PARAM.SFO is not the one pkg.py writes for build %d" % build)
    sfo = read_sfo(files["PARAM.SFO"])
    want = {"TITLE_ID": TITLE_ID, "CATEGORY": "HG", "BOOTABLE": 1, "APP_VER": app_ver(build),
            "TITLE": title(build), "PS3_SYSTEM_VER": PS3_SYSTEM_VER}
    for key, value in want.items():
        if sfo.get(key) != value:
            problems.append("PARAM.SFO %s is %r, not %r" % (key, sfo.get(key), value))


def check_eboot(problems, eboot, elf, work):
    """An NPDRM SELF for this content id, free, that decrypts to the ELF that was signed."""
    if eboot[:4] != b"SCE\0":
        problems.append("USRDIR/EBOOT.BIN is not a SELF")
        return
    path = os.path.join(work, "EBOOT.BIN")
    with open(path, "wb") as f:
        f.write(eboot)
    scetool = os.path.join(QWARK, "scetool.exe")
    info = run([scetool, "-i", path], cwd=QWARK)
    for needle in ("NPDRM Application", "[FREE]", "[EXEC]", "ContentID      " + CONTENT_ID, "Key Revision    0x000A"):
        if needle not in info:
            problems.append("scetool -i on EBOOT.BIN does not show %r" % needle)
    run([scetool, "-d", path, os.path.join(work, "EBOOT.elf")], cwd=QWARK)
    if load_segments(open(elf, "rb").read()) != load_segments(open(os.path.join(work, "EBOOT.elf"), "rb").read()):
        problems.append("EBOOT.BIN does not decrypt to %s" % elf)


def check_sdk_reproduction(problems, files, elf, work, sdk_bin):
    """
    The SDK packs PARAM.SFO, ICON0.PNG, an EBOOT.BIN of its own kind (the same
    ELF, through make_fself_npdrm) and ui.pak; then build_pkg packs the very
    items, metadata and keys the SDK's package holds, and the two must be the
    same bytes. That holds this script's package format to the SDK's own.
    """
    root = os.path.join(work, "sdk", "pkg")
    os.makedirs(os.path.join(root, "USRDIR"))
    for name in ("PARAM.SFO", "ICON0.PNG", "USRDIR/ui.pak"):
        with open(os.path.join(root, *name.split("/")), "wb") as f:
            f.write(files[name])
    run([sdk_tool("make_fself_npdrm", sdk_bin), os.path.abspath(elf), os.path.join(root, "USRDIR", "EBOOT.BIN")])
    conf = os.path.join(work, "sdk", "package.conf")
    with open(conf, "w", newline="\n") as f:
        f.write("Content_ID = %s\nK_licensee = 0x00000000000000000000000000000000\n"
                "DRMType = Free\nContentType = GameExec\nPackageVersion = %s\n" % (CONTENT_ID, PACKAGE_VERSION))
    out = os.path.join(work, "sdk", "out")
    os.makedirs(out)
    run([sdk_tool("make_package_npdrm", sdk_bin), "-o", out, conf, root])
    sdk_blob = open(os.path.join(out, CONTENT_ID + ".pkg"), "rb").read()
    sdk = read_pkg(sdk_blob)
    if not sdk["digests_ok"]:
        problems.append("read_pkg does not follow the SDK's own package's digests")
    again = build_pkg(sdk["items"], sdk["content_id"], sdk["key"], sdk["unused"], sdk["meta"])
    if again != sdk_blob:
        problems.append("build_pkg does not reproduce the SDK's own package")
    # Our metadata is what the SDK writes for the same items, but for the QA digest (7).
    sfo = read_sfo(files["PARAM.SFO"])
    mine = dict(our_metadata(len(pack_items(sdk["items"])), b"\0" * 16, sfo))
    theirs = dict(sdk["meta"])
    for mid in sorted(set(mine) | set(theirs)):
        if mid == 7:
            continue
        if mine.get(mid) != theirs.get(mid):
            problems.append("metadata %d is %s here and %s from the SDK" % (
                mid, (mine.get(mid) or b"").hex(), (theirs.get(mid) or b"").hex()))
    return len(sdk_blob)


def cmd_check(args):
    check_install_h()
    build = qwark_build()
    tool = sdk_tool("make_package_npdrm", args.sdk_bin)
    blob = open(args.pkg, "rb").read()
    problems = []

    # This script's own reader first: the digests, the items and their kinds.
    pkg = read_pkg(blob)
    if not pkg["digests_ok"]:
        problems.append("a digest does not hold")
    if pkg["content_id"] != CONTENT_ID:
        problems.append("the content id is %s" % pkg["content_id"])
    if pkg["total"] != len(blob):
        problems.append("the header says %d bytes; the file has %d" % (pkg["total"], len(blob)))
    kinds = dict((n, f) for n, f, _b in pkg["items"])
    if kinds.get("USRDIR/EBOOT.BIN") != ITEM_SELF or kinds.get("USRDIR") != ITEM_DIR:
        problems.append("EBOOT.BIN or USRDIR is of the wrong kind: %s" % kinds)
    check_contents(problems, dict((n, b) for n, _f, b in pkg["items"] if b is not None), args, build)
    if problems:
        die("%s:\n  %s" % (args.pkg, "\n  ".join(problems)))

    files = dict((n, b) for n, _f, b in pkg["items"] if b is not None)
    if shutil.disk_usage(HERE).free < FREE_MIN:
        die("less than %d MB free; the check writes its files in %s" % (FREE_MIN >> 20, CHECK_DIR))
    shutil.rmtree(CHECK_DIR, ignore_errors=True)
    work = os.path.join(CHECK_DIR, "work")
    os.makedirs(work)
    sdk_size = 0
    try:
        # Then the SDK's reader, on a copy under the content id's name, which is
        # the name it wants a package to have: what it says of the package, its
        # check of the digests and the files, and its listing.
        named = os.path.join(work, CONTENT_ID + ".pkg")
        shutil.copyfile(args.pkg, named)
        info = run([tool, named])
        for needle in ("ContentID        = " + CONTENT_ID, "DRMType          = Free", "ContentType      = GameExec",
                       "# TITLE_ID       : " + TITLE_ID, "# APP_VER        : " + app_ver(build)):
            if needle not in info:
                problems.append("make_package_npdrm does not say %r" % needle)
        if "corrupted" in info:
            problems.append("make_package_npdrm says the package is corrupted")
        checked = run([tool, "--check", named])
        if "checking package validity" not in checked or "Package Check Complete." not in checked:
            problems.append("make_package_npdrm --check did not get through the package")
        complaints = [line.strip() for line in checked.splitlines()
                      if ("!ERROR!" in line and line.strip() != ">> !ERROR!:") or "Illegal" in line or "corrupt" in line]
        unexpected = [c for c in complaints if c not in EXPECTED_COMPLAINTS]
        if unexpected:
            problems.append("make_package_npdrm --check: %s" % "; ".join(unexpected))
        listing = run([tool, "--list", named]).replace("\\", "/")
        kind_words = {ITEM_SELF: "NPDRM SELF", ITEM_RAW: "raw data", ITEM_DIR: "directory"}
        for name, flags, data in pkg["items"]:
            size = 0 if data is None else len(data)
            if not re.search(r"%s:\+\s+%d: %s\s*$" % (re.escape(kind_words[flags]), size, re.escape(name)),
                             listing, re.M):
                problems.append("make_package_npdrm --list does not show %s, %s, %d bytes" % (
                    name, kind_words[flags], size))

        if not problems:
            check_eboot(problems, files["USRDIR/EBOOT.BIN"], args.elf, work)
            sdk_size = check_sdk_reproduction(problems, files, args.elf, work, args.sdk_bin)
    finally:
        shutil.rmtree(CHECK_DIR, ignore_errors=True)

    if problems:
        die("%s:\n  %s" % (args.pkg, "\n  ".join(problems)))
    print("pkg.py: %s checks out: %s, build %d, %d bytes; the SDK reads and lists it, "
          "and the SDK's own %d-byte package of the same files is reproduced byte for byte" % (
              args.pkg, CONTENT_ID, build, len(blob), sdk_size))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("content-id")
    st = sub.add_parser("stage")
    st.add_argument("--sprx", required=True)
    st.add_argument("--pak", required=True)
    st.add_argument("--out", required=True)
    bd = sub.add_parser("build")
    bd.add_argument("--root", required=True)
    bd.add_argument("--out", required=True)
    ck = sub.add_parser("check")
    ck.add_argument("--pkg", required=True)
    ck.add_argument("--sprx", required=True)
    ck.add_argument("--pak", required=True)
    ck.add_argument("--elf", required=True)
    ck.add_argument("--sdk-bin")
    ic = sub.add_parser("icon")
    ic.add_argument("out")
    args = ap.parse_args()

    if args.cmd == "content-id":
        print(CONTENT_ID)
    elif args.cmd == "stage":
        cmd_stage(args)
    elif args.cmd == "build":
        cmd_build(args)
    elif args.cmd == "check":
        cmd_check(args)
    elif args.cmd == "icon":
        with open(args.out, "wb") as f:
            f.write(make_icon())


if __name__ == "__main__":
    main()
