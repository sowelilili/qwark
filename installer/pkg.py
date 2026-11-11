#!/usr/bin/env python3
"""
pkg.py: qwark-installer.pkg, around the compiled EBOOT.BIN: its PARAM.SFO and
ICON0.PNG (installer/icon0.png, RaCMAN Reloaded's icon), the package itself, and
the check of the finished package. `make pkg` runs it. It needs the Python
standard library, and for the check the SDK's own make_package_npdrm and
make_fself_npdrm and qwark's scetool.

From the qwark checkout:

    python -I installer/pkg.py content-id
        the package's content id, which the makefile signs EBOOT.BIN with

    python -I installer/pkg.py stage --sprx qwark.sprx --pak ui.pak --out installer/build
                                     [--games rac1.sprx ...] [--client ../racman-reloaded]
        installer/build/pkg/ holds PARAM.SFO, ICON0.PNG and USRDIR/ with the
        module and the pak (and the game modules, and with --client the
        library of the client's mods and premade savefiles, library.bin);
        EBOOT.BIN goes into USRDIR/ after this

    python -I installer/pkg.py build --root installer/build/pkg --out qwark-installer.pkg
        the package of that folder

    python -I installer/pkg.py check --pkg qwark-installer.pkg --sprx qwark.sprx
                                     --pak ui.pak --elf installer/build/EBOOT.elf
                                     [--games ...] [--client ../racman-reloaded]
        reads the package back and holds every file in it against what went in
        (library.bin expanded file by file, each held to its CRC and to the
        client's own file);
        has the SDK's make_package_npdrm read, check and list it; decrypts
        EBOOT.BIN with scetool and holds its segments against the ELF that was
        signed; and has the SDK pack the same files itself, then packs them
        again here and holds the two packages to each other, byte for byte.
        It never asks the SDK to unpack it: make_package_npdrm --extract never
        finishes an EBOOT.BIN that scetool signed, and writes for ever

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
# Revision 1.26: the game modules (qwark's PROTOCOL.md 9.11), which a package
# carries when it is given them (--games): a test build's, and a release's
# since build 57 (build 56's release had none).
# Each is named for its game, as qwark's title index names it (src/games/titles.c).
GAME_FILES = ["USRDIR/rac1.sprx", "USRDIR/rac2.sprx", "USRDIR/rac3.sprx", "USRDIR/rac4.sprx",
              "USRDIR/tod.sprx"]

# The client's mods and premade savefiles, which a package carries when it is
# given the client's checkout (--client): see "the library" below.
LIBRARY_FILE = "USRDIR/library.bin"

# Items, in the SDK packager's order: PARAM.SFO and ICON0.PNG, the folder, EBOOT.BIN first in it.
ITEM_ORDER = ["PARAM.SFO", "ICON0.PNG", "USRDIR", "USRDIR/EBOOT.BIN", "USRDIR/qwark.sprx", "USRDIR/ui.pak"] + \
    GAME_FILES + [LIBRARY_FILE]

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

# The icon on the XMB is RaCMAN Reloaded's own: installer/icon0.png, 320 x 176, 8-bit
# RGBA, which is the size the XMB shows an ICON0.PNG at. It was made once, from the
# client's icon (racman-reloaded/packaging/racman-reloaded.png, 256 x 256 RGBA), because
# the build has no image library and this script reads the finished file. The icon was
# fitted inside the 320 x 176 and centred without distortion, so it is 176 x 176 at
# x = 72, on a transparent background, scaled by .NET's System.Drawing (PowerShell:
# Graphics.DrawImage with HighQualityBicubic, PixelOffsetMode HighQuality and
# WrapMode TileFlipXY onto a transparent Format32bppArgb 320 x 176 Bitmap, saved as
# PNG). System.Drawing's PNG carries gAMA, sRGB and pHYs chunks, which were taken out
# (IHDR, IDAT recompressed at level 9, IEND are all that is left), so the console's
# decoder has only pixels to read. To change the icon, redo that and replace the file.
ICON_PNG = os.path.join(HERE, "icon0.png")
ICON_W, ICON_H = 320, 176


def read_icon():
    """The bytes of installer/icon0.png, once they are known to be an ICON0.PNG: a PNG
    of 320 x 176, 8-bit RGBA, not interlaced, whose chunk checksums hold and whose
    pixel data is as long as that size says."""
    data = open(ICON_PNG, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        die("%s is not a PNG" % ICON_PNG)
    pos, chunks, idat = 8, [], b""
    while pos + 12 <= len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        crc = struct.unpack(">I", data[pos + 8 + length:pos + 12 + length])[0]
        if len(body) != length or crc != zlib.crc32(kind + body) & 0xFFFFFFFF:
            die("%s: the %s chunk is damaged" % (ICON_PNG, kind.decode("ascii", "replace")))
        chunks.append(kind)
        if kind == b"IDAT":
            idat += body
        elif kind == b"IHDR" and struct.unpack(">IIBBBBB", body) != (ICON_W, ICON_H, 8, 6, 0, 0, 0):
            die("%s is not %d x %d, 8-bit RGBA, not interlaced" % (ICON_PNG, ICON_W, ICON_H))
        pos += 12 + length
    if chunks[:1] != [b"IHDR"] or chunks[-1:] != [b"IEND"] or pos != len(data):
        die("%s is not a whole PNG" % ICON_PNG)
    if len(zlib.decompress(idat)) != ICON_H * (1 + ICON_W * 4):
        die("%s: the pixel data is not %d x %d RGBA" % (ICON_PNG, ICON_W, ICON_H))
    return data


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
            if name in GAME_FILES or name == LIBRARY_FILE:
                continue
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


# ---------------------------------------------------------------- the library

# The mods and premade savefiles RaCMAN Reloaded ships, for a console with no PC:
# USRDIR/library.bin, which the installer expands into the places the client's own
# uploads put them (install.h, "The library"). Taken at build time from the
# client's checkout (--client, the makefile's CLIENT_DIR, ../racman-reloaded by
# default) by the client's own rules, so the package carries what a release of
# that checkout carries and the console gets what the client would have sent:
#
#   mods       the title folders publish.ps1's $ExpectedMods names, but libs/,
#              the old client's Lua helpers, which the client never uploads and
#              qwark never reads; in each, every mod folder with a patch.txt,
#              less those publish.ps1 drops (an "automation:" line, Remove-LuaMods)
#              and those the client never offers (#- visible: false). A mod is
#              what ModLibrary.EnsureUploadedAsync sends: patch.txt, each .bin it
#              names that is there, and qwark.sum, the CRC-32 of patch.txt and
#              those .bin files in that order, as eight lowercase hex digits.
#              The rest of the folder (sources, notes, src.zip) is the PC's.
#   savefiles  premade-savefiles/<TITLEID>/*.sav into
#              savefiles/<TITLEID>/(premade savefiles)/, and for each line of its
#              titles.txt, "TITLEID[/game] folder", that folder's files into
#              savefiles/<TITLEID>/(premade savefiles)[ <game>]/, as
#              PremadeSaveFiles.ConsoleCategory names the category a load
#              uploads into.
#
# A savefile is almost all runs of one byte, so each file is stored in qrle
# (installer/qrle.h) when that is shorter, as it is for every savefile, and as
# it is (stored) otherwise; files with the same bytes share them.

LIB_MAGIC = b"QLIB"
LIB_FORMAT = 1
LIB_HEAD = 32
LIB_ENTRY = 160
LIB_PATH_MAX = 128
LIB_MOD_FILE = 1
LIB_SAVE = 2
LIB_STORED = 0
LIB_QRLE = 1
LIB_MAX = 4 << 20               # install.h INST_LIB_MAX
FILE_MAX = 16 << 20             # install.h INST_FILE_MAX
MOD_NAME_MAX = 31               # ModFolderName.MaxBytes, qwark's mod folder bound
SAVE_NAME_MAX = 32              # qwark's savefile_name_ok
PREMADE_CATEGORY = "(premade savefiles)"   # PremadeSaveFiles.Category
LUA_LIBS = "libs"


class LibraryError(Exception):
    pass


# qrle (installer/qrle.h): tokens of a varint v, kind v & 1, count (v >> 1) + 1;
# a literal's bytes follow it, a run's one byte. A run is worth a token from four.
QRLE_MIN_RUN = 4
_RUN_RE = re.compile(rb"(.)\1{%d,}" % (QRLE_MIN_RUN - 1), re.S)


def _varint(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def qrle_encode(data):
    data = bytes(data)
    out = bytearray()
    lit = 0
    for m in _RUN_RE.finditer(data):
        start, end = m.span()
        if start > lit:
            out += _varint((start - lit - 1) << 1) + data[lit:start]
        out += _varint(((end - start - 1) << 1) | 1) + data[start:start + 1]
        lit = end
    if lit < len(data):
        out += _varint((len(data) - lit - 1) << 1) + data[lit:]
    return bytes(out)


def qrle_decode(stream, size):
    """The expansion, held to every rule qrle.c holds it to; LibraryError for a bad stream."""
    out = bytearray()
    pos = 0
    while pos < len(stream):
        v = shift = 0
        while True:
            if pos >= len(stream) or shift > 28:
                raise LibraryError("qrle: a varint runs off the end or past five bytes")
            b = stream[pos]
            pos += 1
            if shift == 28 and b & 0x70:
                raise LibraryError("qrle: a varint past 32 bits")
            v |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                break
        n = (v >> 1) + 1
        if n > size - len(out):
            raise LibraryError("qrle: a token past the file's size")
        if v & 1:
            if pos >= len(stream):
                raise LibraryError("qrle: a run with no byte")
            out += stream[pos:pos + 1] * n
            pos += 1
        else:
            if n > len(stream) - pos:
                raise LibraryError("qrle: a literal past the end of the stream")
            out += stream[pos:pos + n]
            pos += n
    if len(out) != size:
        raise LibraryError("qrle: %d bytes, not %d" % (len(out), size))
    return bytes(out)


def path_names(path, name_max):
    """How many names a library path has, by install.c's path_names; 0 when the installer would refuse it."""
    names = path.split("/")
    for name in names:
        if not name or name.startswith(".") or len(name.encode("utf-8")) > name_max:
            return 0
        if any(ord(c) < 0x20 or ord(c) > 0x7E or c in "\\:" for c in name):
            return 0
    return len(names)


def expected_mods(client):
    """publish.ps1's $ExpectedMods: the mod library's folders a release copies."""
    script = os.path.join(client, "publish.ps1")
    m = re.search(r"^\$ExpectedMods\s*=\s*@\(([^)]*)\)", open(script, encoding="utf-8-sig").read(), re.M)
    if not m:
        raise LibraryError("%s has no $ExpectedMods" % script)
    return re.findall(r"'([^']+)'", m.group(1))


def read_mod(folder):
    """
    A mod folder as the client reads it (ModLibrary.Read): None for no patch.txt,
    else (visible, needs_lua, [bin names in first appearance order]).
    """
    patch = os.path.join(folder, "patch.txt")
    if not os.path.isfile(patch):
        return None
    text = open(patch, "rb").read().decode("utf-8", "replace")
    variables, bins, needs_lua = {}, [], False
    for raw in text.split("\n"):
        line = raw.rstrip("\r")
        if len(line) > 2 and line.startswith("#-"):
            parts = line.split(":", 1)
            if len(parts) == 2:
                variables[parts[0][2:].strip().lower()] = parts[1].strip()
            continue
        trimmed = line.strip()
        if len(trimmed) < 2 or trimmed[0] == "#":
            continue
        fields = trimmed.split(":", 1)
        if len(fields) < 2:
            continue
        key, value = fields[0].strip(), fields[1].strip()
        if key.lower() == "automation":
            needs_lua = True
            continue
        if not key.lower().startswith("0x"):
            continue
        if not value.lower().startswith("0x") and value and value.lower() not in [b.lower() for b in bins]:
            bins.append(value)
    visible = variables.get("visible", "").lower() != "false"
    return visible, needs_lua, bins


def client_mods(client):
    """[(title, dir, [(name, bytes)])]: each mod the client would upload, its files in upload order, qwark.sum last."""
    mods = []
    for title in expected_mods(client):
        if title == LUA_LIBS:
            continue
        title_dir = os.path.join(client, "mods", title)
        if not os.path.isdir(title_dir):
            raise LibraryError("the client's mods have no %s, which publish.ps1 ships" % title)
        for name in sorted(os.listdir(title_dir)):
            folder = os.path.join(title_dir, name)
            mod = read_mod(folder) if os.path.isdir(folder) else None
            if mod is None:
                continue
            visible, needs_lua, bins = mod
            if needs_lua or not visible:
                continue
            if len(name.encode("utf-8")) > MOD_NAME_MAX:
                raise LibraryError("mods/%s/%s: the folder name is longer than qwark takes" % (title, name))
            files = [("patch.txt", open(os.path.join(folder, "patch.txt"), "rb").read())]
            for b in bins:
                path = os.path.join(folder, *b.replace("\\", "/").split("/"))
                if os.path.isfile(path):
                    files.append((b.replace("\\", "/"), open(path, "rb").read()))
            crc = 0
            for _n, blob in files:
                crc = zlib.crc32(blob, crc)
            files.append(("qwark.sum", ("%08x" % (crc & 0xFFFFFFFF)).encode("ascii")))
            mods.append((title, name, files))
    return mods


def premade_titles(text):
    """titles.txt as PremadeSaveFiles.ParseTitles reads it: {"TITLEID[/game]": folder}."""
    titles = {}
    for raw in text.split("\n"):
        line = raw.strip().lstrip("﻿")
        if not line or line[0] == "#":
            continue
        fields = line.split()
        if len(fields) < 2:
            continue
        folder = "".join(c for c in fields[1].strip() if c not in '<>:"/\\|?*' and ord(c) >= 0x20).strip(" .")
        if folder:
            titles[fields[0].replace("\\", "/").strip("/")] = folder
    return titles


def premade_files(root, folder):
    """PremadeSaveFiles.Files: the .sav files of one folder, sorted without regard to case."""
    path = os.path.join(root, folder)
    if not os.path.isdir(path):
        return []
    names = [n for n in os.listdir(path)
             if n.lower().endswith(".sav") and not n.startswith(".") and os.path.isfile(os.path.join(path, n))]
    return sorted(names, key=str.lower)


def client_saves(client):
    """[(console path under /dev_hdd0/qwark, bytes)], sorted by path."""
    root = os.path.join(client, "premade-savefiles")
    if not os.path.isdir(root):
        raise LibraryError("the client has no premade-savefiles folder at %s" % root)
    titles_path = os.path.join(root, "titles.txt")
    titles = premade_titles(open(titles_path, encoding="utf-8").read()) if os.path.isfile(titles_path) else {}
    # (title id, category) -> the folder its files come from.
    places = {}
    for folder in os.listdir(root):
        if os.path.isdir(os.path.join(root, folder)):
            places[(folder, PREMADE_CATEGORY)] = folder
    for key, folder in titles.items():
        if "/" in key:
            title, game = key.split("/", 1)
            places[(title, "%s %s" % (PREMADE_CATEGORY, game.lower()))] = folder
        else:
            places[(key, PREMADE_CATEGORY)] = folder
    saves = []
    for (title, category), folder in places.items():
        for name in premade_files(root, folder):
            saves.append(("savefiles/%s/%s/%s" % (title, category, name),
                          open(os.path.join(root, folder, name), "rb").read()))
    return sorted(saves)


def library_entries(client):
    """Every file the library carries: [(kind, group, path, bytes)], each mod's files together."""
    entries = []
    for group, (title, name, files) in enumerate(client_mods(client)):
        for fname, blob in files:
            entries.append((LIB_MOD_FILE, group, "mods/%s/%s/%s" % (title, name, fname), blob))
    for path, blob in client_saves(client):
        entries.append((LIB_SAVE, 0, path, blob))
    for kind, _g, path, blob in entries:
        name_max = SAVE_NAME_MAX if kind == LIB_SAVE else MOD_NAME_MAX
        names = path_names(path, name_max)
        if (kind == LIB_SAVE and (names != 4 or not path.endswith(".sav"))) or (kind == LIB_MOD_FILE and names < 4):
            raise LibraryError("%s is not a path the installer writes" % path)
        if len(path.encode("ascii")) >= LIB_PATH_MAX:
            raise LibraryError("%s is longer than a library path" % path)
        if len(blob) > FILE_MAX:
            raise LibraryError("%s is larger than the installer takes" % path)
    return entries


def make_library(entries):
    table = b""
    data = bytearray()
    shared = {}
    for kind, group, path, blob in entries:
        packed = qrle_encode(blob)
        code = LIB_QRLE if len(packed) < len(blob) else LIB_STORED
        if code == LIB_STORED:
            packed = blob
        key = (code, packed)
        if key not in shared:
            shared[key] = len(data)
            data += packed
        table += struct.pack(">BBHIIII12s", kind, code, group, shared[key], len(packed), len(blob),
                             zlib.crc32(blob) & 0xFFFFFFFF, b"") + path.encode("ascii").ljust(LIB_PATH_MAX, b"\0")
    if len(entries) > 0xFFFF:
        raise LibraryError("more files than a library holds")
    head = LIB_MAGIC + struct.pack(">HHII", LIB_FORMAT, len(entries), zlib.crc32(table) & 0xFFFFFFFF, len(data))
    blob = head.ljust(LIB_HEAD, b"\0") + table + bytes(data)
    if len(blob) > LIB_MAX:
        raise LibraryError("the library is %d bytes, more than the installer reads" % len(blob))
    return blob


def read_library(blob):
    """
    A library read back by install.c's rules, each file expanded and held to
    its CRC: [(kind, group, path, bytes)]. LibraryError for anything the
    installer would refuse.
    """
    if len(blob) > LIB_MAX:
        raise LibraryError("larger than the installer reads")
    if len(blob) < LIB_HEAD or blob[:4] != LIB_MAGIC:
        raise LibraryError("not a library")
    fmt, count, table_crc, data_len = struct.unpack_from(">HHII", blob, 4)
    table = blob[LIB_HEAD:LIB_HEAD + LIB_ENTRY * count]
    if fmt != LIB_FORMAT or len(table) != LIB_ENTRY * count or len(blob) - LIB_HEAD - len(table) != data_len:
        raise LibraryError("the header does not describe the file")
    if zlib.crc32(table) & 0xFFFFFFFF != table_crc:
        raise LibraryError("the table's CRC does not hold")
    data = blob[LIB_HEAD + len(table):]
    out, last_group, root = [], -1, None
    for i in range(count):
        kind, code, group, off, length, size, crc, _pad = struct.unpack_from(">BBHIIII12s", table, i * LIB_ENTRY)
        raw_path = table[i * LIB_ENTRY + 32:(i + 1) * LIB_ENTRY]
        if b"\0" not in raw_path:
            raise LibraryError("entry %d: an unterminated path" % i)
        path = raw_path[:raw_path.index(b"\0")].decode("ascii", "replace")
        if off > len(data) or length > len(data) - off or size > FILE_MAX:
            raise LibraryError("%s: outside the data" % path)
        if code == LIB_STORED:
            if length != size:
                raise LibraryError("%s: stored, but not its size" % path)
            body = data[off:off + length]
        elif code == LIB_QRLE:
            body = qrle_decode(data[off:off + length], size)
        else:
            raise LibraryError("%s: code %d" % (path, code))
        if zlib.crc32(body) & 0xFFFFFFFF != crc:
            raise LibraryError("%s: its CRC does not hold" % path)
        if kind == LIB_SAVE:
            if path_names(path, SAVE_NAME_MAX) != 4 or not path.startswith("savefiles/") or \
                    not path.endswith(".sav") or group != 0:
                raise LibraryError("%s: not a savefile's path" % path)
        elif kind == LIB_MOD_FILE:
            if path_names(path, MOD_NAME_MAX) < 4 or not path.startswith("mods/"):
                raise LibraryError("%s: not a mod's path" % path)
            mod_root = "/".join(path.split("/")[:3])
            if group != last_group:
                if group != last_group + 1:
                    raise LibraryError("%s: mod %d after mod %d" % (path, group, last_group))
                last_group, root = group, mod_root
            elif mod_root != root:
                raise LibraryError("%s: outside its mod's folder %s" % (path, root))
        else:
            raise LibraryError("%s: kind %d" % (path, kind))
        out.append((kind, group, path, body))
    return out


def library_summary(entries):
    mods = len(set(g for k, g, _p, _b in entries if k == LIB_MOD_FILE))
    saves = sum(1 for k, _g, _p, _b in entries if k == LIB_SAVE)
    return "%d mods and %d premade savefiles" % (mods, saves)


def check_library(problems, blob, client):
    """The package's library.bin: one the installer takes, holding exactly what the client's checkout ships."""
    try:
        want = library_entries(client)
        got = read_library(blob)
    except LibraryError as e:
        problems.append("USRDIR/library.bin: %s" % e)
        return
    if got != want:
        have = dict((p, b) for _k, _g, p, b in got)
        need = dict((p, b) for _k, _g, p, b in want)
        missing = sorted(set(need) - set(have))
        extra = sorted(set(have) - set(need))
        changed = sorted(p for p in set(need) & set(have) if need[p] != have[p])
        problems.append("USRDIR/library.bin is not %s's: missing %s, extra %s, different %s" % (
            client, missing or "nothing", extra or "nothing", changed or "nothing"))
    elif blob != make_library(want):
        problems.append("USRDIR/library.bin holds the client's files but is not the library pkg.py makes of them")


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
        f.write(read_icon())
    shutil.copyfile(args.sprx, os.path.join(root, "USRDIR", "qwark.sprx"))
    shutil.copyfile(args.pak, os.path.join(root, "USRDIR", "ui.pak"))
    for game in game_paths(args):
        check_sprx(open(game, "rb").read(), game)
        shutil.copyfile(game, os.path.join(root, "USRDIR", os.path.basename(game)))
    library = ""
    if args.client:
        try:
            entries = library_entries(args.client)
            blob = make_library(entries)
        except (LibraryError, OSError) as e:
            die("the library of %s: %s" % (args.client, e))
        with open(os.path.join(root, *LIBRARY_FILE.split("/")), "wb") as f:
            f.write(blob)
        raw = sum(len(b) for _k, _g, _p, b in entries)
        library = ", and %s from %s (%d files, %d bytes in %d)" % (
            library_summary(entries), args.client, len(entries), raw, len(blob))
    print("pkg.py: staged %s for build %d (%s)%s%s" % (root, build, app_ver(build),
          ", with %d game modules" % len(game_paths(args)) if game_paths(args) else "", library))


def game_paths(args):
    """--games: the game modules a package carries, rac1.sprx to rac4.sprx and tod.sprx."""
    out = list(getattr(args, "games", None) or [])
    for g in out:
        if "USRDIR/" + os.path.basename(g) not in GAME_FILES:
            die("%s is not a game module's name (rac1.sprx to rac4.sprx, tod.sprx)" % g)
    return out


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
    games = dict(("USRDIR/" + os.path.basename(g), g) for g in game_paths(args))
    want = PKG_FILES + sorted(games) + ([LIBRARY_FILE] if args.client else [])
    if sorted(files) != sorted(want):
        problems.append("the package holds %s, not %s" % (sorted(files), sorted(want)))
        return
    if args.client:
        check_library(problems, files[LIBRARY_FILE], args.client)
    for name, path in games.items():
        if files[name] != open(path, "rb").read():
            problems.append("%s is not %s" % (name, path))
    if files["USRDIR/qwark.sprx"] != open(args.sprx, "rb").read():
        problems.append("USRDIR/qwark.sprx is not %s" % args.sprx)
    if files["USRDIR/ui.pak"] != open(args.pak, "rb").read():
        problems.append("USRDIR/ui.pak is not %s" % args.pak)
    check_pak(files["USRDIR/ui.pak"], build, "USRDIR/ui.pak")
    if files["ICON0.PNG"] != read_icon():
        problems.append("ICON0.PNG is not installer/icon0.png")
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
        expected = EXPECTED_COMPLAINTS + [">> !ERROR!: USRDIR/%s is not an NPDRM SELF." % os.path.basename(g)
                                          for g in game_paths(args)]
        unexpected = [c for c in complaints if c not in expected]
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
    library = ""
    if args.client:
        library = ", with %s, every file expanded and held to the client's" % library_summary(
            read_library(files[LIBRARY_FILE]))
    print("pkg.py: %s checks out: %s, build %d, %d bytes%s; the SDK reads and lists it, "
          "and the SDK's own %d-byte package of the same files is reproduced byte for byte" % (
              args.pkg, CONTENT_ID, build, len(blob), library, sdk_size))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("content-id")
    st = sub.add_parser("stage")
    st.add_argument("--sprx", required=True)
    st.add_argument("--pak", required=True)
    st.add_argument("--out", required=True)
    st.add_argument("--games", nargs="*", default=[])
    st.add_argument("--client", help="the client's checkout, whose mods and premade savefiles the package carries")
    bd = sub.add_parser("build")
    bd.add_argument("--root", required=True)
    bd.add_argument("--out", required=True)
    ck = sub.add_parser("check")
    ck.add_argument("--pkg", required=True)
    ck.add_argument("--sprx", required=True)
    ck.add_argument("--pak", required=True)
    ck.add_argument("--elf", required=True)
    ck.add_argument("--sdk-bin")
    ck.add_argument("--games", nargs="*", default=[])
    ck.add_argument("--client", help="the client's checkout the package's library was made from")
    args = ap.parse_args()

    if args.cmd == "content-id":
        print(CONTENT_ID)
    elif args.cmd == "stage":
        cmd_stage(args)
    elif args.cmd == "build":
        cmd_build(args)
    elif args.cmd == "check":
        cmd_check(args)


if __name__ == "__main__":
    main()
