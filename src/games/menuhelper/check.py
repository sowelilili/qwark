#!/usr/bin/env python3
"""
check.py: the offline checks of the console menu's renderers, against the
game's executable and every mod we know of. It reads the committed
src/games/menuhelper_bins.c and the headers beside this file, so it needs no
SDK. From the qwark checkout:

    python -I src/games/menuhelper/check.py [--eboots DIR] [--mods LABEL=DIR ...]

For Deadlocked's renderer it checks:
  1. every word the install replaces holds, in the shipped executable, the
     original the table carries (the install refuses anything else);
  2. each hook word is a bl from its site to the stub its comment names, and
     each stub starts the way that stub does;
  3. screen 36's start and stop are descriptors in the data image, each
     {code in the code image, the renderer's TOC};
  4. the code decodes as it should: every branch stays inside it but the two
     tail branches into the game's own functions, no sc, no vector
     instruction, no write to r13, and every indirect call switches r2 through
     one of the game's descriptors the header names and back;
  5. each of those descriptors is {the code the header names, TOC 0x9BCD58};
  6. the code area is in the text segment's last page past its end, the data
     area in the read-write segment's last page past its end, both inside the
     areas the header gives, and nothing in the executable names either;
  7. the data image starts with the mailbox as menubox.h lays it out and the
     renderer state with its magic;
  8. the two leaf stubs, stepped instruction by instruction on a model of the
     game's memory, do what the C in test/test_menu.c assumes of them;
  9. none of the caves or hook words overlaps a word or a cave of any mod in
     the libraries (the client's shipped mods, the old racman library, the
     user's own) or any of qwark's own patches, but for probe N0, the
     renderer's prototype, which shares its hook sites by design.
Exit status 0 when everything passes.
"""
import argparse
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
QWARK = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))   # the qwark checkout
GEN_C = os.path.join(QWARK, "src", "games", "menuhelper_bins.c")
MENUBOX = os.path.join(QWARK, "src", "core", "menubox.h")

DL_TOC = 0x009BCD58

# qwark's own writes into Deadlocked, build 54: src/games/rac4*.{c,h},
# src/games/sfhelper/sf_rac4.h, src/games/switches/sw_rac4.ld. (lo, hi, what).
QWARK_RAC4 = [
    (0x00667BCC, 0x0066840C, "code switches (RPCS3), fast-load movie, savefile helper"),
    (0x00707430, 0x00707434, "savefile helper hook"),
    (0x015CD71D, 0x015CD780, "savefile helper request bytes"),
    (0x015CD780, 0x015CD783, "code switch flags (RPCS3)"),
    (0x01600000, 0x01700000, "savefile aside buffer"),
    (0x01700000, 0x01700004, "quit flag"),
    (0x01710000, 0x01710004, "loading flag"),
    (0x00013780, 0x0001378C, "quit hook"),
    (0x00011884, 0x00011888, "loading hook"),
    (0x00011904, 0x0001192C, "loading hook trampoline"),
    (0x0080883C, 0x00808840, "crash patch"),
    (0x0051E1E8, 0x0051E250, "crash patches"),
    (0x00618008, 0x00618070, "crash patches"),
    (0x001D7D18, 0x001D7D1C, "refill ammo"),
    (0x002A8AE8, 0x002A8AF8, "fast loads"),
    (0x002A8C50, 0x002A8C60, "fast loads"),
    (0x002A8CC8, 0x002A8CCC, "fast loads"),
]

# Overlaps that are by design: (mod folder) -> why.
ACCEPTED = {
    "probe-n0": "probe N0 is this renderer's prototype and hooks the same sites; "
                "the install refuses while it is loaded, and N0 never needs loading again",
}

# The stubs' first words (mh_rac4_stubs.S).
STUB_FIRST = {
    "mh_frame_stub": (0xFFFFFFFF, 0xF821FF81, "stdu r1,-0x80(r1)"),
    "mh_options_stub": (0xFFFFFFFF, 0xF821FF71, "stdu r1,-0x90(r1)"),
    "mh_inject_stub": (0xFFFF0000, 0x3D600000, "lis r11,..."),
    "mh_screen_stub": (0xFFFFFFFF, 0x38800001, "li r4,1"),
}


# ------------------------------------------------------------------ inputs

def header_defs(path):
    defs = {}
    for line in open(path, encoding="utf-8").read().splitlines():
        m = re.match(r"^#define\s+(\w+)\s+(0x[0-9A-Fa-f]+|\d+|\"[^\"]*\")\s*(/\*(.*)\*/)?\s*$", line)
        if m:
            v = m.group(2)
            if v.startswith('"'):
                defs[m.group(1)] = v.strip('"')
            else:
                defs[m.group(1)] = int(v, 0)
            if m.group(4):
                defs[m.group(1) + "#comment"] = m.group(4).strip()
    return defs


def parse_bins(path, g):
    text = open(path, encoding="utf-8").read()

    def array(name):
        m = re.search(r"static const u8 %s\[\] = \{(.*?)\};" % name, text, re.S)
        if not m:
            return None
        return bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))

    data = array("mh_%s_data" % g)
    code = array("mh_%s_code" % g)
    m = re.search(r"static const struct mh_word mh_%s_hooks\[\] = \{(.*?)\};" % g, text, re.S)
    hooks = []
    if m:
        for line in m.group(1).splitlines():
            w = re.match(r"\s*\{ (0x[0-9A-Fa-f]+)u, (0x[0-9A-Fa-f]+)u, (0x[0-9A-Fa-f]+)u \},?\s*/\*\s*(.*?)\s*\*/", line)
            if w:
                hooks.append((int(w.group(1), 16), int(w.group(2), 16), int(w.group(3), 16), w.group(4)))
    m = re.search(r"static const struct mh_cave mh_%s_caves\[\] = \{(.*?)\};" % g, text, re.S)
    caves = []
    if m:
        for c in re.finditer(r"\{ (0x[0-9A-Fa-f]+)u, (\w+), \(u32\)sizeof\(\w+\), (\d) \}", m.group(1)):
            caves.append((int(c.group(1), 16), c.group(2), int(c.group(3))))
    return data, code, hooks, caves


class Elf:
    def __init__(self, path):
        d = open(path, "rb").read()
        self.d = d
        phoff = struct.unpack(">Q", d[0x20:0x28])[0]
        phentsize, phnum = struct.unpack(">HH", d[0x36:0x3A])
        self.segs = []
        for i in range(phnum):
            o = phoff + i * phentsize
            p_type, p_flags, p_offset, p_vaddr, _, p_filesz, p_memsz = \
                struct.unpack(">IIQQQQQ", d[o:o + 48])
            if p_type == 1 and p_memsz:
                self.segs.append((p_vaddr, p_filesz, p_memsz, p_offset, p_flags))
        self.text = self.segs[0]
        self.rw = self.segs[1]

    def word(self, a):
        for v, fs, ms, off, fl in self.segs:
            if v <= a < v + ms:
                if a + 4 <= v + fs:
                    return struct.unpack(">I", self.d[off + a - v:off + a - v + 4])[0]
                return 0
        return None

    def words(self, seg):
        v, fs, ms, off, fl = seg
        for k in range(0, fs - 3, 4):
            yield v + k, struct.unpack(">I", self.d[off + k:off + k + 4])[0]


def branch_target(a, w):
    op = w >> 26
    if op == 18:
        li = w & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        return (li if (w & 2) else a + li) & 0xFFFFFFFF
    if op == 16:
        bd = w & 0xFFFC
        if bd & 0x8000:
            bd -= 0x10000
        return (bd if (w & 2) else a + bd) & 0xFFFFFFFF
    return None


def page_end(a):
    return (a + 0xFFFF) & ~0xFFFF


def refs_into(elf, lo, hi):
    """Branches into [lo, hi) from outside, words naming it, lis/addi pairs naming it."""
    branches, words, pairs = [], [], []
    lis = {}
    for a, w in elf.words(elf.text):
        t = branch_target(a, w)
        if t is not None and lo <= t < hi:
            branches.append((a, t))
        op = w >> 26
        if op == 15 and ((w >> 16) & 31) == 0:
            lis[(w >> 21) & 31] = (w & 0xFFFF, a)
        elif op in (12, 14, 24, 32, 34, 36, 38, 40, 44, 48, 52):
            src = (w >> 21) & 31 if op == 24 else (w >> 16) & 31
            if src in lis and a - lis[src][1] <= 128:
                imm = w & 0xFFFF
                hiw = lis[src][0] << 16
                val = (hiw | imm) if op == 24 else hiw + (imm - 0x10000 if imm & 0x8000 else imm)
                val &= 0xFFFFFFFF
                if lo <= val < hi:
                    pairs.append((a, val))
    for seg in elf.segs:
        for a, w in elf.words(seg):
            if lo <= w < hi:
                words.append((a, w))
    return branches, words, pairs


def mod_entries(roots, title):
    out = []
    for label, root in roots:
        tdir = os.path.join(root, title)
        if not os.path.isdir(tdir):
            continue
        for m in sorted(os.listdir(tdir)):
            mdir = os.path.join(tdir, m)
            p = os.path.join(mdir, "patch.txt")
            if not os.path.isfile(p):
                continue
            for line in open(p, encoding="utf-8", errors="replace").read().splitlines():
                line = line.strip()
                if len(line) < 2 or line.startswith("#") or ":" not in line:
                    continue
                a, v = [x.strip() for x in line.split(":", 1)]
                if not a.lower().startswith("0x"):
                    continue
                try:
                    addr = int(a, 16)
                except ValueError:
                    continue
                if v.lower().startswith("0x"):
                    out.append((addr, addr + 4, label, m, "word"))
                else:
                    f = os.path.join(mdir, v)
                    n = os.path.getsize(f) if os.path.isfile(f) else 0
                    out.append((addr, addr + n, label, m, "cave " + v))
    return out


def mod_pointers(roots, title, lo, hi):
    """Every lis/addi (or lis/ori, or lis/load-store) pair in a mod's cave files naming [lo, hi)."""
    found = []
    for label, root in roots:
        tdir = os.path.join(root, title)
        if not os.path.isdir(tdir):
            continue
        for m in sorted(os.listdir(tdir)):
            for dp, _, files in os.walk(os.path.join(tdir, m)):
                for f in files:
                    if not f.endswith(".bin"):
                        continue
                    b = open(os.path.join(dp, f), "rb").read()
                    lis = {}
                    for i in range(0, len(b) - 3, 4):
                        w = struct.unpack(">I", b[i:i + 4])[0]
                        op = w >> 26
                        if op == 15 and ((w >> 16) & 31) == 0:
                            lis[(w >> 21) & 31] = w & 0xFFFF
                        elif op in (14, 24, 32, 34, 36, 38, 40, 44, 48, 52, 58, 62):
                            ra = (w >> 21) & 31 if op == 24 else (w >> 16) & 31
                            if ra in lis:
                                imm = w & 0xFFFF
                                v = ((lis[ra] << 16) | imm) if op == 24 else                                     (lis[ra] << 16) + (imm - 0x10000 if imm & 0x8000 else imm)
                                if lo <= (v & 0xFFFFFFFF) < hi:
                                    found.append("%s/%s %s: 0x%08X" % (label, m, f, v & 0xFFFFFFFF))
    return found


def lint_code(code, base, allowed=()):
    bad = []
    for i in range(len(code) // 4):
        a = base + 4 * i
        w = struct.unpack(">I", code[4 * i:4 * i + 4])[0]
        op = w >> 26
        t = branch_target(a, w)
        if t is not None and not base <= t < base + len(code) and t not in allowed:
            bad.append("0x%08X %08X branches out of the image to 0x%08X" % (a, w, t))
        if op == 17:
            bad.append("0x%08X %08X is a system call" % (a, w))
        if op == 4:
            bad.append("0x%08X %08X is a vector instruction" % (a, w))
        rd = (w >> 21) & 31
        ra = (w >> 16) & 31
        if op in (14, 15, 32, 33, 34, 35, 40, 41, 42, 43, 58) and rd == 13:
            bad.append("0x%08X %08X writes r13" % (a, w))
        if op in (24, 25, 26, 27, 28, 29) and ra == 13:
            bad.append("0x%08X %08X writes r13" % (a, w))
        if op == 31 and ra == 13 and ((w >> 1) & 0x3FF) in (444, 28, 60, 124, 316, 412, 476, 954, 922, 26):
            bad.append("0x%08X %08X writes r13" % (a, w))
    return bad


def calls_via_descriptors(code, base, known):
    words = [struct.unpack(">I", code[i:i + 4])[0] for i in range(0, len(code) - 3, 4)]
    bad, seen = [], set()
    for i, w in enumerate(words):
        if w != 0x4E800421:                        # bctrl
            continue
        a = base + 4 * i
        window = words[max(0, i - 40):i]
        lwz_toc = [x for x in window if (x >> 26) == 32 and ((x >> 21) & 31) == 2 and (x & 0xFFFF) == 4]
        std_toc = 0xF8410028 in window             # std r2,40(r1)
        after = words[i + 1] if i + 1 < len(words) else 0
        if not lwz_toc or not std_toc or after != 0xE8410028:   # ld r2,40(r1)
            bad.append("0x%08X: bctrl without the r2 switch and restore" % a)
            continue
        base_reg = (lwz_toc[-1] >> 16) & 31
        hi = lo = None
        for x in window:
            op = x >> 26
            if op == 15 and ((x >> 21) & 31) == base_reg and ((x >> 16) & 31) == 0:
                hi, lo = x & 0xFFFF, None
            elif op == 24 and ((x >> 16) & 31) == base_reg and ((x >> 21) & 31) == base_reg and hi is not None:
                lo = x & 0xFFFF
        if hi is not None and lo is not None:
            d = (hi << 16) | lo
            seen.add(d)
            if d not in known:
                bad.append("0x%08X: calls through 0x%08X, not a descriptor the header names" % (a, d))
    return bad, seen


# -------------------------------------------------------- the leaf stubs, stepped

class Ppc:
    """Just enough PowerPC to step the two leaf stubs."""

    def __init__(self, code, base, mem):
        self.code, self.base, self.mem = code, base, mem
        self.r = [0] * 32
        self.cr0 = 0

    def ld(self, a, n):
        return int.from_bytes(bytes(self.mem.get(a + i, 0) for i in range(n)), "big")

    def st(self, a, v, n):
        for i, b in enumerate((v & ((1 << (8 * n)) - 1)).to_bytes(n, "big")):
            self.mem[a + i] = b

    def run(self, pc, steps=200):
        for _ in range(steps):
            w = struct.unpack(">I", self.code[pc - self.base:pc - self.base + 4])[0]
            op, rd, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
            imm = w & 0xFFFF
            simm = imm - 0x10000 if imm & 0x8000 else imm
            base = self.r[ra] if ra else 0
            if op == 15:                                    # addis / lis
                self.r[rd] = (base + (simm << 16)) & 0xFFFFFFFF
            elif op == 14:                                  # addi / li
                self.r[rd] = (base + simm) & 0xFFFFFFFF
            elif op == 24:                                  # ori (rS, rA swapped)
                self.r[ra] = self.r[rd] | imm
            elif op == 34:                                  # lbz
                self.r[rd] = self.ld((base + simm) & 0xFFFFFFFF, 1)
            elif op == 32:                                  # lwz
                self.r[rd] = self.ld((base + simm) & 0xFFFFFFFF, 4)
            elif op == 38:                                  # stb
                self.st((base + simm) & 0xFFFFFFFF, self.r[rd], 1)
            elif op == 36:                                  # stw
                self.st((base + simm) & 0xFFFFFFFF, self.r[rd], 4)
            elif op == 11 and (rd >> 2) == 0:               # cmpwi cr0
                a = self.r[ra] - (1 << 32) if self.r[ra] & 0x80000000 else self.r[ra]
                self.cr0 = -1 if a < simm else (1 if a > simm else 0)
            elif w == 0x4E800020:                           # blr
                return True
            elif w == 0x4D820020:                           # beqlr
                if self.cr0 == 0:
                    return True
            elif w == 0x4C820020:                           # bnelr
                if self.cr0 != 0:
                    return True
            else:
                raise ValueError("0x%08X: %08X is not an instruction the stepper knows" % (pc, w))
            pc += 4
        raise ValueError("the stub did not return")


def check_stubs(ok, code, cave, hdr, box_defs, stub_at):
    mbox = hdr["MH_DATA"]
    force = mbox + hdr["MH_STATE_OFF"] + hdr["ST_FORCE36"]
    injects = mbox + hdr["MH_STATE_OFF"] + hdr["ST_INJECTS"]
    opens = mbox + hdr["MH_STATE_OFF"] + hdr["ST_COMBO_OPENS"]
    request = mbox + box_defs["MBOX_OFF_REQUEST"]
    gtype = hdr["DL_GAME_TYPE"]
    hero, pad = 0x010D4430, 0x00B11E80
    hud = pad + hdr["DL_PAD_HUDBITS"]

    def inject(req, game_type=0, pad_ptr=pad, force_before=1):
        cpu = Ppc(code, cave, {})
        cpu.st(request, req, 1)
        cpu.st(force, force_before, 1)
        cpu.st(gtype, game_type, 4)
        cpu.st(hero + hdr["DL_HERO_PAD"], pad_ptr, 4)
        cpu.st(hud, 0x40, 4)
        cpu.st(injects, 5, 4)
        cpu.r[3] = hero
        cpu.run(stub_at["mh_inject_stub"])
        return cpu

    c = inject(0)
    ok(c.ld(force, 1) == 0 and c.ld(hud, 4) == 0x40 and c.r[3] == hero,
       "inject, no request: FORCE36 cleared, the pad untouched, r3 kept")
    c = inject(1)
    ok(c.ld(request, 1) == 0 and c.ld(hud, 4) == 0x840 and c.ld(force, 1) == 1 and
       c.ld(injects, 4) == 6 and c.r[3] == hero,
       "inject, an open request: taken, Start or'd into hudBitsOn, FORCE36 up, counted, r3 kept")
    c = inject(1, game_type=1)
    ok(c.ld(request, 1) == 0 and c.ld(hud, 4) == 0x40 and c.ld(force, 1) == 0,
       "inject in multiplayer: the request is taken and nothing pressed")
    c = inject(2)
    ok(c.ld(request, 1) == 0 and c.ld(hud, 4) == 0x40 and c.ld(force, 1) == 0,
       "inject, a close request: dropped, nothing pressed")
    c = inject(1, pad_ptr=0)
    ok(c.ld(request, 1) == 0 and c.ld(force, 1) == 0, "inject with no pad: nothing pressed")

    for f, want in ((0, 1), (1, hdr["MH_SCREEN_ID"])):
        cpu = Ppc(code, cave, {})
        cpu.st(force, f, 1)
        cpu.st(opens, 2, 4)
        cpu.r[3] = 0
        cpu.r[4] = 0xDEAD
        cpu.run(stub_at["mh_screen_stub"])
        ok(cpu.r[4] == want and cpu.r[3] == 0 and cpu.ld(force, 1) == 0 and
           cpu.ld(opens, 4) == (3 if f else 2),
           "screen stub, FORCE36 %d: r4 = %d, r3 kept, the flag down%s" %
           (f, want, ", counted" if f else ""))


# --------------------------------------------------------------- the checks

def check_rac4(ok, elf, roots):
    hdr = header_defs(os.path.join(HERE, "mh_rac4.h"))
    box_defs = header_defs(MENUBOX)
    data, code, hooks, caves = parse_bins(GEN_C, "rac4")
    print("\nNPEA00423 console menu renderer")
    if data is None or code is None:
        ok(False, "menuhelper_bins.c has rac4's data and code")
        return

    cave, cave_end = hdr["MH_CAVE"], hdr["MH_CAVE_END"]
    dat, dat_end = hdr["MH_DATA"], hdr["MH_DATA_END"]
    tv, tfs, tms, _, _ = elf.text
    rv, rfs, rms, _, _ = elf.rw

    ok(caves == [(dat, "mh_rac4_data", 0), (cave, "mh_rac4_code", 1)],
       "two caves, the data image at 0x%08X then the code at 0x%08X" % (dat, cave))
    ok(cave + len(code) <= cave_end and dat + len(data) <= dat_end,
       "code %d bytes (of %d), data %d bytes (of %d)" % (len(code), cave_end - cave, len(data), dat_end - dat))

    # 6. the areas
    ok(cave >= tv + tms and cave_end <= page_end(tv + tms),
       "code area 0x%08X..0x%08X is past the text segment's end (0x%08X), in its last page" %
       (cave, cave_end, tv + tms))
    ok(dat >= rv + rms and dat_end <= page_end(rv + rms),
       "data area 0x%08X..0x%08X is past the read-write segment's end (0x%08X), in its last page" %
       (dat, dat_end, rv + rms))
    # Both areas are past their segments' ends, where the executable has no
    # bytes at all, so no instruction of the game can be meant to go there: a
    # word of data in the text segment that happens to decode as a branch into
    # one is not code. What would matter is a pointer to one, as a word or as a
    # lis/addi pair, and there is none.
    for lo, hi, what in ((cave, cave_end, "code"), (dat, dat_end, "data")):
        br, wd, pr = refs_into(elf, lo, hi)
        ok(not wd and not pr, "no word and no lis/addi pair in the executable names the %s area%s" %
           (what, "" if not br else " (%d data words decode as branches into it)" % len(br)))

    # 7. the data image
    magic, version, size = struct.unpack(">IHH", data[0:8])
    ok(magic == box_defs["MBOX_MAGIC"] and version == box_defs["MBOX_VERSION"] and size == hdr["MH_MBOX_SIZE"],
       "the data image starts with the mailbox: 'QMNU', version %d, %d bytes" % (version, size))
    ok(data[8] == hdr["MH_ROWS"] and data[9] == box_defs["MBOX_ROW_SIZE"] and data[10] == hdr["MH_ROWS"] and
       data[11] == hdr["MH_CAPS"] and data[12] == 0 and data[13] == 0 and data[14] == hdr["MH_GAME_ID"],
       "row_cap 8, row_size 0x40, max_rows 8, caps 0x%02X, closed, no request, renderer 4" % data[11])
    ok(box_defs["MBOX_OFF_ROWS"] + hdr["MH_ROWS"] * box_defs["MBOX_ROW_SIZE"] == hdr["MH_MBOX_SIZE"] and
       hdr["MH_MBOX_SIZE"] <= hdr["MH_STATE_OFF"], "the mailbox's eight rows end before the renderer state")
    ok(struct.unpack(">I", data[hdr["MH_STATE_OFF"]:hdr["MH_STATE_OFF"] + 4])[0] == hdr["MH_MAGIC"] and
       all(b == 0 for b in data[16:hdr["MH_STATE_OFF"]]),
       "the state at MH_STATE with its magic 'QMH4', everything else zero")
    ok(hdr["MH_STATE_OFF"] + hdr["ST_SIZE"] <= len(data) and
       hdr["ST_STAGE"] + hdr["MH_MBOX_SIZE"] - box_defs["MBOX_OFF_PAGE_KEY"] == hdr["ST_SIZE"],
       "the state, %d bytes, is all in the image and holds a whole page" % hdr["ST_SIZE"])

    # 1. the originals, and 2. the hook words
    sites = {
        hdr["MH_SITE_FRAME"]: ("mh_frame_stub", hdr["MH_ORIG_FRAME"]),
        hdr["MH_SITE_SCREEN"]: ("mh_screen_stub", hdr["MH_ORIG_SCREEN"]),
        hdr["MH_SITE_INJECT"]: ("mh_inject_stub", hdr["MH_ORIG_INJECT"]),
        hdr["MH_SITE_OPTIONS"]: ("mh_options_stub", hdr["MH_ORIG_OPTIONS"]),
    }
    slot = hdr["MH_SCREEN_SLOT"]
    ok(len(hooks) == 8 and [h[0] for h in hooks] ==
       [slot, slot + 4, slot + 8, slot + 12, hdr["MH_SITE_FRAME"], hdr["MH_SITE_SCREEN"],
        hdr["MH_SITE_INJECT"], hdr["MH_SITE_OPTIONS"]],
       "eight words: screen 36's slot, then the frame, screen, inject and Options hooks")
    for a, value, orig, what in hooks:
        ok(elf.word(a) == orig, "0x%08X holds 0x%08X in the executable (%s)" % (a, orig, what))

    ok(elf.word(hdr["MH_SITE_INJECT"] - 4) is not None and (elf.word(hdr["MH_SITE_INJECT"] - 4) >> 26) == 18 and
       branch_target(hdr["MH_SITE_INJECT"] - 4, elf.word(hdr["MH_SITE_INJECT"] - 4)) == 0x002E2E48,
       "the inject site is the nop after GuiHud's call to GB_GetLocalHeroFromSlot (0x2E2E48)")
    ok(elf.word(hdr["MH_SITE_FRAME"] + 4) == 0x480000BC and
       branch_target(hdr["MH_SITE_FRAME"], hdr["MH_ORIG_FRAME"]) == hdr["GAME_HANDLE_MENU_INPUT"],
       "the frame site calls HandleMenuInput and goes on to UpdateMenus' end")
    ok(branch_target(hdr["MH_SITE_OPTIONS"], hdr["MH_ORIG_OPTIONS"]) == hdr["GAME_SET_MENU_BUTTONS"],
       "the Options site calls SetMenuButtons")

    stub_at = {}
    for a, value, orig, what in hooks:
        if a not in sites:
            continue
        stub, _ = sites[a]
        t = branch_target(a, value) if (value >> 26) == 18 and (value & 3) == 1 else None
        first = struct.unpack(">I", code[t - cave:t - cave + 4])[0] if t is not None and cave <= t < cave + len(code) else None
        mask, want, name = STUB_FIRST[stub]
        good = t is not None and first is not None and (first & mask) == want and stub in what
        ok(good, "0x%08X: %08X = bl 0x%08X, %s (%s)" % (a, value, t or 0, stub, name))
        if good:
            stub_at[stub] = t

    # 3. the slot
    words = dict((h[0], h[1]) for h in hooks)
    ok(words[slot] == 0 and words[slot + 4] == 0, "screen 36 has no init and no reset")
    for off, what in ((8, "start"), (12, "stop")):
        desc = words[slot + off]
        if dat <= desc < dat + len(data) - 8:
            c, t = struct.unpack(">II", data[desc - dat:desc - dat + 8])
            ok(cave <= c < cave + len(code) and dat + 0x8000 <= t <= dat + len(data) + 0x8000,
               "its %s 0x%08X is a descriptor {0x%08X in the code, TOC 0x%08X}" % (what, desc, c, t))
        else:
            ok(False, "its %s 0x%08X is in the data image" % (what, desc))

    # 4. the code, 5. the descriptors
    allowed = (hdr["GAME_HANDLE_MENU_INPUT"], hdr["GAME_SET_MENU_BUTTONS"])
    bad = lint_code(code, cave, allowed=allowed)
    ok(not bad, "the code: no branch out but the two tail branches, no sc, no vector instruction, no write to r13")
    for b in bad[:12]:
        print("        " + b)
    outs = [cave + i for i in range(0, len(code), 4)
            if branch_target(cave + i, struct.unpack(">I", code[i:i + 4])[0]) in allowed]
    ok(len(outs) == 2 and all((struct.unpack(">I", code[o - cave:o - cave + 4])[0] & 1) == 0 for o in outs),
       "exactly two ways out, both plain branches (no link): the stubs' tail calls")

    opds = dict((v, k) for k, v in hdr.items() if k.startswith("OPD_") and not k.endswith("#comment"))
    bad, seen = calls_via_descriptors(code, cave, set(opds))
    ok(not bad, "every indirect call switches r2 through one of the header's descriptors and back")
    for b in bad[:12]:
        print("        " + b)
    ok(seen == set(opds), "and every descriptor the header names is called (%d of %d)" % (len(seen), len(opds)))
    for name in sorted(opds.values()):
        a = hdr[name]
        m = re.search(r"(0x[0-9A-Fa-f]+)\s*$", hdr.get(name + "#comment", ""))
        fn = int(m.group(1), 16) if m else None
        ok(fn is not None and elf.word(a) == fn and elf.word(a + 4) == DL_TOC,
           "%s 0x%08X = {0x%08X, TOC 0x%08X}" % (name, a, fn or 0, DL_TOC))

    # 8. the leaf stubs, stepped
    if "mh_inject_stub" in stub_at and "mh_screen_stub" in stub_at:
        try:
            check_stubs(ok, code, cave, hdr, box_defs, stub_at)
        except ValueError as e:
            ok(False, "the leaf stubs step: %s" % e)

    # 9. overlaps
    ours = [(cave, cave + len(code), "renderer code"), (dat, dat + len(data), "renderer data")]
    ours += [(a, a + 4, "renderer word") for a, _, _, _ in hooks]
    known = mod_entries(roots, "NPEA00423")
    clashes, accepted = [], []
    for lo, hi, what in ours:
        for a, b, label, mod, kind in known:
            if a < hi and b > lo:
                text = "%s 0x%08X..0x%08X vs %s/%s %s 0x%08X..0x%08X" % (what, lo, hi, label, mod, kind, a, b)
                (accepted if mod in ACCEPTED else clashes).append((text, mod))
        for a, b, w in QWARK_RAC4:
            if a < hi and b > lo:
                clashes.append(("%s 0x%08X..0x%08X vs qwark %s 0x%08X..0x%08X" % (what, lo, hi, w, a, b), None))
    ok(not clashes, "no overlap with %d mod words and caves, nor with qwark's %d own" %
       (len(known), len(QWARK_RAC4)))
    for c, _ in clashes:
        print("        " + c)
    # A mod's code can keep words of its own in free memory without a cave
    # there (the display mods at 0x17DD000..0x17DD600, Stereo Music at
    # 0x17DF000..0x17DF0A8): every address a mod's code builds in either area.
    named = [x for x in mod_pointers(roots, "NPEA00423", cave, cave_end) +
             mod_pointers(roots, "NPEA00423", dat, dat_end)
             if not any(x.startswith("%s/%s " % (l, a)) for l, _ in roots for a in ACCEPTED)]
    ok(not named, "no mod's code names an address in either area")
    for x in named[:12]:
        print("        " + x)
    for c, mod in accepted:
        print("        known: %s: %s" % (c, ACCEPTED[mod]))


def find_up(rel):
    d = QWARK
    for _ in range(8):
        c = os.path.join(d, rel)
        if os.path.exists(c):
            return c
        d = os.path.dirname(d)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--eboots", default=None)
    ap.add_argument("--mods", action="append", default=None,
                    help="LABEL=DIR, a mods folder with <TITLEID>/<mod>/patch.txt")
    args = ap.parse_args()

    eboots = args.eboots
    if eboots is None:
        e = find_up(os.path.join("eboots", "NPEA00423.elf"))
        eboots = os.path.dirname(e) if e else None
    if not eboots:
        print("check: no eboots folder (rewrite/eboots/NPEA00423.elf); pass --eboots")
        return 2

    roots = []
    if args.mods:
        for m in args.mods:
            label, _, path = m.partition("=")
            roots.append((label, path))
    else:
        shipped = find_up(os.path.join("racman-reloaded", "mods"))
        if shipped:
            roots.append(("shipped", shipped))
            legacy = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(shipped))),
                                  "legacy", "racman-official", "mods")
            roots.append(("legacy", legacy))
        appdata = os.environ.get("APPDATA")
        if appdata:
            roots.append(("user", os.path.join(appdata, "RaCMAN Reloaded", "mods")))
    roots = [(l, p) for l, p in roots if os.path.isdir(p)]

    failures = 0

    def ok(cond, msg):
        nonlocal failures
        print(("  ok    " if cond else "  FAIL  ") + msg)
        if not cond:
            failures += 1

    print("mod libraries: " + (", ".join("%s (%s)" % (l, p) for l, p in roots) or "none"))
    check_rac4(ok, Elf(os.path.join(eboots, "NPEA00423.elf")), roots)

    print("\n%s" % ("all checks passed" if failures == 0 else "%d check(s) FAILED" % failures))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
