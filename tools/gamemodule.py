"""gamemodule.py: a game module's layout and checks (revision 1.26, PROTOCOL.md 9.11).

    python -I tools/gamemodule.py objs <game objects...>
        no object holds writable data or TLS (what gcc marks writable for its
        own reasons, .opd, .toc, .got and .data.rel.ro*, goes into the one R-E
        segment, which lv2 relocates)
    python -I tools/gamemodule.py merge <in.prx> <out.prx>
        the fixup's two LOAD segments, R-E and RW-, become one R-E segment
    python -I tools/gamemodule.py check <merged.prx> <merged unstripped.prx> --game N --id 0xID [--name F]
        one R-X LOAD segment of at most 61,440 bytes, no writable or TLS
        section beyond the relocated ones, no import, and the stamp
        (src/plat/ps3/game_module.c) naming this build (src/core/proto.h's
        QWARK_BUILD), this build ID and this game; F, the module's file name
        (rac1.sprx ..), is what the line it prints calls it
    python -I tools/gamemodule.py info <file.prx>
        the program headers and the page count

Why a post-process: the SDK's ppu-lv2-prx-fixup lays a PRX out by section
name, .opd, .toc and every .data* into a second, writable segment, whatever the
sections' flags say, and neither renaming nor its segment options change that
(SPEC-game-modules.md 7.2; the PRX probe's console-ui-probes c-prx/dummies/
prxmerge.py, which this follows). The fixup already places the second segment
straight after the first in the file and in memory, so merging moves no byte:

  - LOAD 0 grows to cover LOAD 1, its flags R-E as they were (the few bytes
    of alignment past LOAD 1's file bytes, an empty .bss's, included);
  - LOAD 1's program header goes, and the relocation segment (PT 0x700000A4)
    moves up into its slot;
  - every PRX relocation (24 bytes: offset u64, u16 0, u8 value segment, u8
    target segment, type u32, addend s64) that named segment 1 names segment 0,
    its offset or addend moved by segment 1's distance from segment 0.

lv2 then writes the same relocations with the same values, into the R-E
segment, as it does for qwark's own .lib.ent and module info: the PRX probe's
D3 and D4 showed it on CFW and on HEN.

Stdlib only; safe under python -I. Inputs over 16 MB are refused.
"""
import os
import re
import struct
import sys

PT_LOAD = 1
PT_PRX_RELA = 0x700000A4
PF_W = 0x2
PF_X = 0x1
PF_SPU_W = 0x00200000
SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_TLS = 0x400
SHT_NOBITS = 8
PAGE = 0x10000
MAX_MEMSZ = 61440          # a 4 KB margin under one page
STAMP_MAGIC = 0x51474D53   # "QGMS", src/plat/ps3/game_module.c

# What gcc marks writable for its own reasons, which the merge puts in the R-E segment.
RELOCATED = (".opd", ".toc", ".got")
# A PRX's imports; a game module has none.
IMPORTS = (".lib.stub", ".rodata.sceFNID", ".data.sceFStub", ".data.sceVStub", ".rodata.sceVNID",
           ".sceStub.text")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def die(msg):
    sys.stderr.write("gamemodule.py: %s\n" % msg)
    sys.exit(1)


def read(path):
    with open(path, "rb") as f:
        data = bytearray(f.read(16 * 1024 * 1024 + 1))
    if len(data) > 16 * 1024 * 1024:
        die("%s is over 16 MB" % path)
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 2:
        die("%s is not a big-endian ELF64 file" % path)
    return data


def phdrs(data):
    phoff, = struct.unpack_from(">Q", data, 0x20)
    phentsize, phnum = struct.unpack_from(">HH", data, 0x36)
    if phentsize != 56 or phoff + phnum * 56 > len(data):
        die("bad program header table")
    return phoff, [list(struct.unpack_from(">IIQQQQQQ", data, phoff + i * 56)) for i in range(phnum)]


def sections(data):
    shoff, = struct.unpack_from(">Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from(">HHH", data, 0x3A)
    if shoff == 0 or shnum == 0:
        return []
    if shentsize != 64 or shoff + shnum * 64 > len(data):
        die("bad section header table")
    hdr = [struct.unpack_from(">IIQQQQIIQQ", data, shoff + i * 64) for i in range(shnum)]
    stro = hdr[shstrndx][4]
    out = []
    for h in hdr[1:]:
        end = data.index(b"\0", stro + h[0])
        out.append((data[stro + h[0]:end].decode("ascii", "replace"), h[1], h[2], h[5]))
    return out


def relocated(name):
    return name in RELOCATED or name.startswith(".data.rel.ro") or name.startswith(".opd") or \
        name.startswith(".toc")


def objs(paths):
    bad = []
    for path in paths:
        for name, _type, flags, size in sections(read(path)):
            if flags & SHF_TLS and size:
                bad.append("%s: %s is TLS" % (os.path.basename(path), name))
            elif flags & SHF_WRITE and size:
                if relocated(name) or name.startswith(".psp_lib_mark"):
                    continue
                bad.append("%s: %s holds %d writable bytes" % (os.path.basename(path), name, size))
    if bad:
        die("a game module keeps its state in the lent area (src/games/module.h): " + "; ".join(bad))
    print("gamemodule.py: %d objects, no writable data and no TLS" % len(paths))


def merge(src, dst):
    data = read(src)
    phoff, ph = phdrs(data)
    loads = [i for i, p in enumerate(ph) if p[0] == PT_LOAD]
    relas = [i for i, p in enumerate(ph) if p[0] == PT_PRX_RELA]
    if loads != [0, 1] or relas != [2] or len(ph) != 3:
        die("expected LOAD, LOAD, PRX relocations; found types %s" % [hex(p[0]) for p in ph])
    t, d, r = ph
    if t[1] & PF_W or not t[1] & PF_X:
        die("segment 0 is not R-E (flags 0x%x)" % t[1])
    delta = d[3] - t[3]
    if d[2] - t[2] != delta:
        die("segment 1 is not where segment 0's file layout continues (file 0x%x, memory 0x%x)"
            % (d[2] - t[2], delta))
    if t[3] + t[6] > d[3]:
        die("segment 0 overlaps segment 1")
    if t[5] != t[6]:
        die("segment 0 has memory past its file bytes")
    if d[1] & PF_X:
        die("segment 1 is executable (flags 0x%x): not the fixup's data segment" % d[1])
    if d[6] - d[5] > 16:
        die("segment 1 has %d bytes of bss: a game module has no writable statics" % (d[6] - d[5]))

    roff, rsize = r[2], r[5]
    if rsize % 24 or roff + rsize > len(data):
        die("bad relocation segment")
    moved = 0
    for j in range(rsize // 24):
        at = roff + j * 24
        off, zero, ival, iaddr, rtype, addend = struct.unpack_from(">QHBBIq", data, at)
        if iaddr > 1 or (ival > 1 and ival != 0xFF):
            die("relocation %d names a segment past 1 (%d, %d)" % (j, iaddr, ival))
        if iaddr == 1:
            iaddr, off = 0, off + delta
            moved += 1
        if ival == 1:
            ival, addend = 0, addend + delta
            moved += 1
        struct.pack_into(">QHBBIq", data, at, off, zero, ival, iaddr, rtype, addend)

    merged = list(t)
    merged[5] = d[2] + d[5] - t[2]
    merged[6] = delta + d[6]
    struct.pack_into(">IIQQQQQQ", data, phoff, *merged)
    struct.pack_into(">IIQQQQQQ", data, phoff + 56, *r)
    struct.pack_into(">IIQQQQQQ", data, phoff + 112, *([0] * 8))
    struct.pack_into(">H", data, 0x38, 2)
    with open(dst, "wb") as f:
        f.write(data)
    print("gamemodule.py: %s -> %s: one R-E segment, memsz %d (was %d + %d), %d relocation fields moved"
          % (os.path.basename(src), os.path.basename(dst), merged[6], t[6], d[6], moved))


def build_number():
    with open(os.path.join(ROOT, "src", "core", "proto.h"), "r") as f:
        m = re.search(r"^#define QWARK_BUILD\s+(\d+)", f.read(), re.M)
    if not m:
        die("no QWARK_BUILD in src/core/proto.h")
    return int(m.group(1))


def check(prx, sym, game, build_id, module=None):
    data = read(prx)
    _phoff, ph = phdrs(data)
    loads = [p for p in ph if p[0] == PT_LOAD]
    if len(loads) != 1:
        die("%s has %d LOAD segments; a game module has one" % (prx, len(loads)))
    seg = loads[0]
    if seg[1] & (PF_W | PF_SPU_W) or not seg[1] & PF_X:
        die("%s's segment is not R-X (flags 0x%x)" % (prx, seg[1]))
    if seg[6] > MAX_MEMSZ:
        die("%s is %d bytes, over the %d a game module may be (one 64 KB page, 4 KB to spare)"
            % (prx, seg[6], MAX_MEMSZ))
    for name, _type, flags, size in sections(read(sym)):
        if not size or not flags & SHF_ALLOC:
            continue
        # .lib.stub.top and .btm are the empty list's two markers, in every PRX.
        if any(name.startswith(i) for i in IMPORTS) and not name.endswith((".top", ".btm")):
            die("%s imports (%s, %d bytes): a game module calls the core through core_api alone"
                % (sym, name, size))
        if flags & SHF_TLS:
            die("%s has TLS (%s)" % (sym, name))
        # The fixup folds .data.rel.ro* into .data, so the objects are what `objs`
        # holds to having no writable data; what is left to refuse is any bss.
        if flags & SHF_WRITE and _type == SHT_NOBITS:
            die("%s has bss (%s, %d bytes)" % (sym, name, size))
    want = struct.pack(">IIII", STAMP_MAGIC, build_number(), build_id, game)
    if bytes(data).find(want) < 0:
        found = bytes(data).find(struct.pack(">I", STAMP_MAGIC))
        what = ("none" if found < 0 else "build %d, ID 0x%08X, game %d"
                % struct.unpack_from(">III", data, found + 4))
        die("%s's stamp is not build %d, ID 0x%08X, game %d (it has %s)"
            % (prx, build_number(), build_id, game, what))
    print("gamemodule.py: %s: one R-X segment of %d bytes (%d spare in its page), "
          "no writable data, no import, build %d, ID 0x%08X"
          % (module or ("game %d" % game), seg[6], PAGE - seg[6], build_number(), build_id))


def info(path):
    data = read(path)
    _phoff, ph = phdrs(data)
    pages = 0
    for i, p in enumerate(ph):
        rwx = ("R" if p[1] & 4 else "-") + ("W" if p[1] & 2 else "-") + ("E" if p[1] & 1 else "-")
        kind = {PT_LOAD: "LOAD", PT_PRX_RELA: "PRX-RELA"}.get(p[0], hex(p[0]))
        n = (p[6] + PAGE - 1) // PAGE if p[0] == PT_LOAD else 0
        pages += n
        print("  %d %-8s %s flags 0x%08x vaddr 0x%x filesz 0x%x memsz %d pages %d"
              % (i, kind, rwx, p[1], p[3], p[5], p[6], n))
    print("  %s: %d page(s) of 64 KB" % (path, pages))


def main(argv):
    if len(argv) >= 3 and argv[1] == "objs":
        objs(argv[2:])
    elif len(argv) == 4 and argv[1] == "merge":
        merge(argv[2], argv[3])
    elif len(argv) in (8, 10) and argv[1] == "check" and argv[4] == "--game" and argv[6] == "--id" and \
            (len(argv) == 8 or argv[8] == "--name"):
        check(argv[2], argv[3], int(argv[5]), int(argv[7], 0), argv[9] if len(argv) == 10 else None)
    elif len(argv) == 3 and argv[1] == "info":
        info(argv[2])
    else:
        die(__doc__.split("\n\n")[0])


if __name__ == "__main__":
    main(sys.argv)
