#!/usr/bin/env python3
"""
check.py: the offline checks of the console menu's renderers, against the
game's executable and every mod we know of. It reads the committed
src/games/menuhelper_bins.c and the headers beside this file, so it needs no
SDK. From the qwark checkout:

    python -I src/games/menuhelper/check.py [--eboots DIR] [--mods LABEL=DIR ...]

A renderer is the shared core (mh_core.c) and a game's shim; the core is
checked once, as part of every renderer, and the shim against its game.

For the core, in every renderer:
  0. it names no game address: no indirect call at all, and no constant it
     builds outside the renderer's own mailbox and data areas.

For Deadlocked's renderer:
  1. every word the install replaces holds, in the shipped executable, the
     original the table carries (the install refuses anything else);
  2. each hook word is a bl from its site to the stub its comment names, and
     each stub starts the way that stub does;
  3. screen 36's start and stop are descriptors in the data image, each
     {code in the code image, the renderer's TOC};
  4. the code decodes as it should: every branch stays inside it but the one
     tail branch into the game's own HandleMenuInput, no sc, no vector
     instruction, no write to r13, and every indirect call switches r2 through
     one of the game's descriptors the header names and back;
  4a. the game's own menus are left as they are but for the Help row: no hook
     word or cave in MenuMain_StartOptions, where round 1 added its RaCMAN row,
     no address in MenuMain_PlayerData built anywhere in the code, and the two
     Help words are the main menu's Help row's label call and its callback's
     screen, a callback nothing else names (so multiplayer, whose main menu
     has no Help, keeps every word);
  4b. what the shim takes from the game is what the Cheats screen uses: its
     templates, its plates, CtlList's vtable;
  5. each of those descriptors is {the code the header names, TOC 0x9BCD58};
  6. the code area is in the text segment's last page past its end, the
     mailbox and data areas in the read-write segment's last page past its
     end, all inside the areas the header gives, and nothing in the executable
     names any of them;
  7. the mailbox image is the header menubox.h lays out, and the data image
     starts with the core's state and its magic;
  8. the leaf stubs, stepped instruction by instruction on a model of the
     game's memory, do what the C in test/test_menu.c assumes of them;
  9. none of the caves or hook words overlaps a word or a cave of any mod in
     the libraries (the client's shipped mods, the old racman library, the
     user's own, the hardware probes as built) or any of qwark's own patches,
     but for probe N0, the renderer's prototype, which shares its hook sites
     by design.

For Up Your Arsenal's renderer, the same, and for its thirteen words: each
holds the game's word in the executable, the code words are branches to the
stubs their comments name, the HELP rows' words are the text id 0xF000 and a
descriptor of the renderer's; the code around each site is what the stubs
assume (the msg_string entry, the main pause init's three tables and bands,
the Start test and path, the launcher); the renderer writes nothing of the
main pause's tables but the HELP rows' two words; the grid configuration is
CHEATS'; the label, inject, combo and rows stubs, stepped, do what the C and
the tests assume; and nothing overlaps the mods, the probes or qwark's own
writes (the trophy caves and the fast-load cave at 0x9807F0..0x9808B0), but
for probes N3 and N3b, the prototypes, whose words are the same by design.
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
SHIM_H = os.path.join(HERE, "mh_shim.h")

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
    "mh_inject_stub": (0xFFFF0000, 0x3D600000, "lis r11,..."),
    "mh_screen_stub": (0xFFFFFFFF, 0x38800001, "li r4,1"),
    "mh_help_label": (0xFFFF0000, 0x3C600000, "lis r3,..."),
}

# The game's own menus, which the renderer must leave as the game has them
# (the user's call: a row added to a list a player knows moves its buttons).
# MenuMain_StartOptions builds the Options list; its `bl SetMenuButtons` at
# 0x3A6F2C (word 0x4BFC456D) is where round 1 put its RaCMAN row.
DL_START_OPTIONS = (0x003A6858, 0x003A6F74)
DL_OPTIONS_SETBUTTONS = (0x003A6F2C, 0x4BFC456D)
# MenuMain_PlayerData, 0x1F8 bytes a player for four: Options' rows and cursor.
DL_MENU_PLAYER = (0x0119F9A8, 0x0119F9A8 + 4 * 0x1F8)


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

    box = array("mh_%s_box" % g)
    state = array("mh_%s_state" % g)
    rest = array("mh_%s_data" % g)
    code = array("mh_%s_code" % g)
    m = re.search(r"/\* %s: the code, .*?the core \((0x[0-9A-Fa-f]+)\.\.(0x[0-9A-Fa-f]+)\)" % g, text)
    core = (int(m.group(1), 16), int(m.group(2), 16)) if m else None
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
        for c in re.finditer(r"\{ (0x[0-9A-Fa-f]+)u, (\w+), \(u32\)sizeof\(\w+\), (\d), (\d+)u \}", m.group(1)):
            caves.append((int(c.group(1), 16), c.group(2), int(c.group(3)), int(c.group(4))))
    # The images as the game holds them: each cave's bytes, then its zeros.
    zeros = dict((name, z) for _, name, _, z in caves)
    data = None
    if box is not None:
        box = box + bytes(zeros.get("mh_%s_box" % g, 0))
    if state is not None and rest is not None:
        data = state + bytes(zeros.get("mh_%s_state" % g, 0)) + rest
    return box, data, code, hooks, caves, core


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
                            # A register the instruction writes no longer holds the lis: a load
                            # through it, `lwz r7,lo(r7)`, leaves a value, not the address.
                            dest = (w >> 16) & 31 if op == 24 else (w >> 21) & 31
                            if op in (14, 24, 32, 34, 40, 42, 58):
                                lis.pop(dest, None)
    return found


def code_pointers(code, base, lo, hi):
    """
    Every lis/addi, lis/ori or lis/load-store pair in the code that names [lo, hi),
    and every addis/load-store pair whose two immediates do: an array indexed at
    run time comes out as `addis rD,rIndex,hi` and `stw rS,lo(rD)`.
    """
    found, lis = [], {}
    for i in range(0, len(code) - 3, 4):
        w = struct.unpack(">I", code[i:i + 4])[0]
        op = w >> 26
        if op == 15:
            lis[(w >> 21) & 31] = w & 0xFFFF
        elif op in (14, 24, 32, 34, 36, 38, 40, 44, 48, 52, 58, 62):
            ra = (w >> 21) & 31 if op == 24 else (w >> 16) & 31
            if ra in lis:
                imm = w & 0xFFFF
                v = ((lis[ra] << 16) | imm) if op == 24 else \
                    (lis[ra] << 16) + (imm - 0x10000 if imm & 0x8000 else imm)
                if lo <= (v & 0xFFFFFFFF) < hi:
                    found.append("0x%08X: 0x%08X" % (base + i, v & 0xFFFFFFFF))
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


def calls_via_descriptors(code, base, known, elf, toc=DL_TOC, code_hi=0x980000):
    """
    Every bctrl switches r2 through a descriptor (lwz r2,4(rB) after std r2,40(r1))
    and takes it back after (ld r2,40(r1)). The compiler hoists a descriptor's
    address out of a loop and merges two calls into one bctrl, so which
    descriptor a bctrl uses is not read off the words before it; instead every
    address the code builds that is one of the game's descriptors ({code, the
    game's TOC}) must be one the header names, and those are what it can call.
    """
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
    # Every lis rB,hi then ori/addi rB,rB,lo: the addresses the code builds.
    lis = {}
    for i, w in enumerate(words):
        op, rd, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        if op == 15 and ra == 0:
            lis[rd] = w & 0xFFFF
            continue
        if op == 24 and rd in lis and ra == rd:               # ori rB,rB,lo
            v = (lis.pop(rd) << 16) | (w & 0xFFFF)
        elif op == 14 and ra in lis and rd == ra:             # addi rB,rB,lo
            imm = w & 0xFFFF
            v = ((lis.pop(ra) << 16) + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF
        else:
            if op in (14, 15, 24, 31) and rd in lis:
                lis.pop(rd)
            continue
        c = elf.word(v)
        if c is None or elf.word(v + 4) != toc or not 0x10000 <= c < code_hi:
            continue
        if v in known:
            seen.add(v)
        else:
            bad.append("0x%08X: builds the game's descriptor 0x%08X, which the header does not name" %
                       (base + 4 * i, v))
    return bad, seen


# -------------------------------------------------------- the leaf stubs, stepped

class Ppc:
    """
    Just enough PowerPC to step the leaf stubs. `written` collects the registers
    (and cr0) the stub wrote, so a check can hold it to the ones its site leaves
    free. run() answers ("blr", None) for a return, or ("b", target) for a plain
    branch out of the code, which ends the stub.
    """

    def __init__(self, code, base, mem):
        self.code, self.base, self.mem = code, base, mem
        self.r = [0] * 32
        self.cr0 = 0
        self.written = set()

    def ld(self, a, n):
        return int.from_bytes(bytes(self.mem.get(a + i, 0) for i in range(n)), "big")

    def st(self, a, v, n):
        for i, b in enumerate((v & ((1 << (8 * n)) - 1)).to_bytes(n, "big")):
            self.mem[a + i] = b

    def set(self, reg, v):
        self.r[reg] = v & 0xFFFFFFFF
        self.written.add("r%d" % reg)

    def cmp(self, a, b):
        self.cr0 = -1 if a < b else (1 if a > b else 0)
        self.written.add("cr0")

    def cond(self, bo, bi):
        """The branch condition BO/BI on cr0: 12 branches if the bit is set, 4 if clear."""
        bit = (self.cr0 < 0, self.cr0 > 0, self.cr0 == 0)[bi]
        if bo == 12:
            return bit
        if bo == 4:
            return not bit
        if bo == 20:
            return True
        raise ValueError("branch BO %d is not one the stepper knows" % bo)

    def run(self, pc, steps=200):
        for _ in range(steps):
            w = struct.unpack(">I", self.code[pc - self.base:pc - self.base + 4])[0]
            op, rd, ra, rb = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
            imm = w & 0xFFFF
            simm = imm - 0x10000 if imm & 0x8000 else imm
            base = self.r[ra] if ra else 0
            signed = lambda v: v - (1 << 32) if v & 0x80000000 else v   # noqa: E731
            if op == 15:                                    # addis / lis
                self.set(rd, base + (simm << 16))
            elif op == 14:                                  # addi / li
                self.set(rd, base + simm)
            elif op == 7:                                   # mulli
                self.set(rd, signed(self.r[ra]) * simm)
            elif op == 24:                                  # ori (rS, rA swapped)
                self.set(ra, self.r[rd] | imm)
            elif op == 28:                                  # andi. (rS, rA swapped)
                self.set(ra, self.r[rd] & imm)
                self.cmp(signed(self.r[ra]), 0)
            elif op == 21:                                  # rlwinm rA,rS,SH,MB,ME
                sh, mb, me = rb, (w >> 6) & 31, (w >> 1) & 31
                v = ((self.r[rd] << sh) | (self.r[rd] >> (32 - sh))) & 0xFFFFFFFF if sh else self.r[rd]
                mask = 0
                for b in range(32):
                    inside = (mb <= b <= me) if mb <= me else (b >= mb or b <= me)
                    if inside:
                        mask |= 1 << (31 - b)
                self.set(ra, v & mask)
                if w & 1:
                    self.cmp(signed(self.r[ra]), 0)
            elif op == 31 and ((w >> 1) & 0x3FF) == 266:    # add
                self.set(rd, self.r[ra] + self.r[rb])
            elif op == 34:                                  # lbz
                self.set(rd, self.ld((base + simm) & 0xFFFFFFFF, 1))
            elif op == 32:                                  # lwz
                self.set(rd, self.ld((base + simm) & 0xFFFFFFFF, 4))
            elif op == 38:                                  # stb
                self.st((base + simm) & 0xFFFFFFFF, self.r[rd], 1)
            elif op == 36:                                  # stw
                self.st((base + simm) & 0xFFFFFFFF, self.r[rd], 4)
            elif op == 11 and (rd >> 2) == 0:               # cmpwi cr0
                self.cmp(signed(self.r[ra]), simm)
            elif op == 10 and (rd >> 2) == 0:               # cmplwi cr0
                self.cmp(self.r[ra], imm)
            elif op == 16 and (w & 3) == 0 and ra < 3:      # bc on cr0 (beq, bne, bgt, ble ...)
                if self.cond(rd, ra):
                    pc = (pc + simm) & 0xFFFFFFFF
                    continue
            elif op == 18 and (w & 3) == 0:                 # b
                li = w & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                t = (pc + li) & 0xFFFFFFFF
                if not self.base <= t < self.base + len(self.code):
                    return ("b", t)
                pc = t
                continue
            elif w == 0x4E800020:                           # blr
                return ("blr", None)
            elif (w & 0xFC00FFFF) == 0x4C000020 and ra < 3: # bclr on cr0 (beqlr, bnelr, blelr ...)
                if self.cond(rd, ra):
                    return ("blr", None)
            else:
                raise ValueError("0x%08X: %08X is not an instruction the stepper knows" % (pc, w))
            pc += 4
        raise ValueError("the stub did not return")


def parse_rest(path, g):
    text = open(path, encoding="utf-8").read()
    m = re.search(r"static const u8 mh_%s_data\[\] = \{(.*?)\};" % g, text, re.S)
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))) if m else b""


def check_stubs(ok, code, cave, hdr, box_defs, shim_defs, stub_at, data):
    force = hdr["MH_SHIM"] + hdr["SH_FORCE36"]
    injects = hdr["MH_STATE"] + shim_defs["ST_OPEN_REQS"]
    opens = hdr["MH_STATE"] + shim_defs["ST_OPENS"]
    request = hdr["MH_BOX"] + box_defs["MBOX_OFF_REQUEST"]
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

    cpu = Ppc(code, cave, {})
    cpu.r = [0x1000 + i for i in range(32)]
    keep = list(cpu.r)
    cpu.run(stub_at["mh_help_label"])
    lab = cpu.r[3]
    text = b""
    if hdr["MH_DATA"] <= lab < hdr["MH_DATA"] + len(data):
        text = data[lab - hdr["MH_DATA"]:].split(b"\0")[0]
    ok(text == b"RACMAN" and all(cpu.r[i] == keep[i] for i in range(32) if i != 3),
       "help label stub: r3 = 0x%08X, \"%s\" in the data image, every other register kept" %
       (lab, text.decode("ascii", "replace")))


# --------------------------------------------------------------- the checks

def check_rac4(ok, elf, roots):
    hdr = header_defs(os.path.join(HERE, "mh_rac4.h"))
    box_defs = header_defs(MENUBOX)
    shim_defs = header_defs(SHIM_H)
    box, data, code, hooks, caves, core = parse_bins(GEN_C, "rac4")
    print("\nNPEA00423 console menu renderer")
    if box is None or data is None or code is None or core is None:
        ok(False, "menuhelper_bins.c has rac4's mailbox, data, code and the core's place in it")
        return

    cave, cave_end = hdr["MH_CAVE"], hdr["MH_CAVE_END"]
    mbx, mbx_end = hdr["MH_BOX"], hdr["MH_BOX_END"]
    dat, dat_end = hdr["MH_DATA"], hdr["MH_DATA_END"]
    tv, tfs, tms, _, _ = elf.text
    rv, rfs, rms, _, _ = elf.rw

    ok(len(caves) == 4 and [c[:3] for c in caves] ==
       [(mbx, "mh_rac4_box", 0), (dat, "mh_rac4_state", 0), (caves[2][0], "mh_rac4_data", 0),
        (cave, "mh_rac4_code", 1)] and caves[2][3] == 0 and caves[3][3] == 0,
       "four caves: the mailbox at 0x%08X, the state at 0x%08X, the read-only data, then the code at 0x%08X" %
       (mbx, dat, cave))
    rest = parse_rest(GEN_C, "rac4")
    ok(caves[0][3] > 0 and caves[1][3] > 0 and caves[2][0] == dat + len(data) - len(rest),
       "the mailbox and the state are a few bytes and then zeros, %d and %d of them, which the module "
       "carries as counts; the read-only data follows the state" % (caves[0][3], caves[1][3]))
    ok(cave + len(code) <= cave_end and mbx + len(box) <= mbx_end and dat + len(data) <= dat_end,
       "code %d bytes (of %d), mailbox %d bytes (of %d), data %d bytes (of %d)" %
       (len(code), cave_end - cave, len(box), mbx_end - mbx, len(data), dat_end - dat))

    # 0. the core
    cs, ce = core
    ok(cave <= cs <= ce <= cave + len(code), "the core is 0x%08X..0x%08X, %d bytes of the code" % (cs, ce, ce - cs))
    body = code[cs - cave:ce - cave]
    words = [struct.unpack(">I", body[i:i + 4])[0] for i in range(0, len(body) - 3, 4)]
    ok(0x4E800421 not in words,
       "the core calls no function through a pointer: it reaches the game only through the shim")
    # The core does arithmetic on its own areas' addresses: an array indexed at
    # run time comes out as `addis rD,rIndex,hi` and a load with the rest, and
    # `end - 1` as the page's start less one, so a value in or just under the
    # areas' page is the core's own. Its magic ('QM..') is a number, not an
    # address. Anything else inside the game's segments would be a game address.
    page_lo, page_hi = (mbx & 0xFFFF0000) - 0x10000, (dat_end + 0xFFFF) & 0xFFFF0000
    named = [x for x in code_pointers(body, cs, 0x00010000, 0xFFFFFFFF)
             if not page_lo <= int(x.split(": ")[1], 16) <= page_hi and
             (int(x.split(": ")[1], 16) >> 16) != (hdr["MH_MAGIC"] >> 16)]
    ok(not named, "and builds no address but its own areas'")
    for x in named[:12]:
        print("        " + x)

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
    ok(mbx >= rv + rms and mbx_end <= page_end(rv + rms),
       "mailbox area 0x%08X..0x%08X is past the read-write segment's end, in its last page" % (mbx, mbx_end))
    for lo, hi, what in ((cave, cave_end, "code"), (mbx, mbx_end, "mailbox"), (dat, dat_end, "data")):
        br, wd, pr = refs_into(elf, lo, hi)
        ok(not wd and not pr, "no word and no lis/addi pair in the executable names the %s area%s" %
           (what, "" if not br else " (%d data words decode as branches into it)" % len(br)))

    # 7. the mailbox and the data image
    magic, version, size = struct.unpack(">IHH", box[0:8])
    ok(len(box) == hdr["MH_MBOX_SIZE"] and magic == box_defs["MBOX_MAGIC"] and
       version == box_defs["MBOX_VERSION"] and size == hdr["MH_MBOX_SIZE"],
       "the mailbox image: 'QMNU', version %d, %d bytes" % (version, size))
    ok(box[8] == hdr["MH_ROWS"] and box[9] == hdr["MH_ROW_BYTES"] == box_defs["MBOX_ROW_SIZE_DESC"] and
       box[10] == hdr["MH_MAIN_ROWS"] and box[11] == hdr["MH_CAPS"] and box[12] == 0 and box[13] == 0 and
       box[14] == hdr["MH_GAME_ID"] and box[15] == hdr["MH_CFG_ROWS"],
       "row_cap %d, rows of 0x%02X with descriptions, %d rows a MAIN page, %d a CONFIG page, caps 0x%02X, "
       "closed, no request, renderer 4" % (box[8], box[9], box[10], box[15], box[11]))
    ok(box_defs["MBOX_OFF_ROWS"] + hdr["MH_ROWS"] * hdr["MH_ROW_BYTES"] == hdr["MH_MBOX_SIZE"] and
       all(b == 0 for b in box[16:]), "the rows fill the image, everything after the header zero")
    shim_off = hdr["MH_SHIM"] - hdr["MH_STATE"]
    ok(struct.unpack(">I", data[0:4])[0] == hdr["MH_MAGIC"] and all(b == 0 for b in data[4:shim_off + hdr["SH_SIZE"]]),
       "the data image: the core's state at MH_STATE with its magic 'QMH4', the shim's at MH_SHIM "
       "(+0x%X), everything else zero" % shim_off)
    ok(shim_off + hdr["SH_SIZE"] <= len(data), "the shim's state, %d bytes, is all in the image" % hdr["SH_SIZE"])

    # 1. the originals, and 2. the hook words
    sites = {
        hdr["MH_SITE_FRAME"]: ("mh_frame_stub", hdr["MH_ORIG_FRAME"]),
        hdr["MH_SITE_SCREEN"]: ("mh_screen_stub", hdr["MH_ORIG_SCREEN"]),
        hdr["MH_SITE_INJECT"]: ("mh_inject_stub", hdr["MH_ORIG_INJECT"]),
    }
    slot = hdr["MH_SCREEN_SLOT"]
    sites[hdr["MH_SITE_HELP_LABEL"]] = ("mh_help_label", hdr["MH_ORIG_HELP_LABEL"])
    ok(len(hooks) == 9 and [h[0] for h in hooks] ==
       [slot, slot + 4, slot + 8, slot + 12, hdr["MH_SITE_FRAME"], hdr["MH_SITE_SCREEN"],
        hdr["MH_SITE_INJECT"], hdr["MH_SITE_HELP_LABEL"], hdr["MH_SITE_HELP_SCREEN"]],
       "nine words: screen 36's slot, then the frame, screen and inject hooks, then the Help row's two")
    for a, value, orig, what in hooks:
        ok(elf.word(a) == orig, "0x%08X holds 0x%08X in the executable (%s)" % (a, orig, what))

    ok(elf.word(hdr["MH_SITE_INJECT"] - 4) is not None and (elf.word(hdr["MH_SITE_INJECT"] - 4) >> 26) == 18 and
       branch_target(hdr["MH_SITE_INJECT"] - 4, elf.word(hdr["MH_SITE_INJECT"] - 4)) == 0x002E2E48,
       "the inject site is the nop after GuiHud's call to GB_GetLocalHeroFromSlot (0x2E2E48)")
    ok(elf.word(hdr["MH_SITE_FRAME"] + 4) == 0x480000BC and
       branch_target(hdr["MH_SITE_FRAME"], hdr["MH_ORIG_FRAME"]) == hdr["GAME_HANDLE_MENU_INPUT"],
       "the frame site calls HandleMenuInput and goes on to UpdateMenus' end")
    # The Start path: GoToScreen(0, r4) then PushState(0, 2), whose menu thread
    # init (0x36270C) empties the screen stack, so triangle at the root closes.
    ok(elf.word(hdr["MH_SITE_SCREEN"] + 4) == 0x48003EA5 and
       branch_target(hdr["MH_SITE_SCREEN"] + 4, elf.word(hdr["MH_SITE_SCREEN"] + 4)) == 0x00361F1C and
       elf.word(hdr["MH_SITE_SCREEN"] + 12) == 0x38600000 and
       elf.word(hdr["MH_SITE_SCREEN"] + 16) == 0x38800002 and
       branch_target(hdr["MH_SITE_SCREEN"] + 20, elf.word(hdr["MH_SITE_SCREEN"] + 20)) == 0x0035FF48,
       "the screen site feeds GuiMenu_GoToScreen (0x361F1C), then GuiMain_PushState(0, 2) (0x35FF48)")

    # 4a. the Help row: the label call and the callback's screen, nothing else
    hl, hs = hdr["MH_SITE_HELP_LABEL"], hdr["MH_SITE_HELP_SCREEN"]
    ok(branch_target(hl, elf.word(hl)) == 0x00198A5C and (elf.word(hl - 4) >> 26) == 31 and
       (elf.word(hl - 8) >> 26) == 40,
       "0x%08X is MenuMain_StartMain's call to msg_string for a row's label (lhz, extsh, bl)" % hl)
    near = [elf.word(hl + 4 * i) for i in range(1, 12)]
    ok(any((w >> 16) == 0x3CA0 and (w & 0xFFFF) == hdr["DL_HELP_CB_OPD"] >> 16 for w in near) and
       any((w >> 16) == 0x30A5 and (w & 0xFFFF) == hdr["DL_HELP_CB_OPD"] & 0xFFFF for w in near),
       "the row it labels is the one whose callback is MenuMain_MainCMDHelpCB's descriptor 0x%08X" %
       hdr["DL_HELP_CB_OPD"])
    ok(elf.word(hdr["DL_HELP_CB_OPD"]) == hdr["DL_HELP_CB"] and elf.word(hdr["DL_HELP_CB_OPD"] + 4) == DL_TOC and
       hdr["DL_HELP_CB"] < hs < hdr["DL_HELP_CB"] + 0x80 and
       branch_target(hs + 16, elf.word(hs + 16)) == 0x00362308,
       "0x%08X is the Help callback's li r4,30 before GuiMenu_PushScreen (0x362308)" % hs)
    br, wd, pr = refs_into(elf, hdr["DL_HELP_CB_OPD"], hdr["DL_HELP_CB_OPD"] + 8)
    ok(not wd and len(pr) == 1 and hl < pr[0][0] < hl + 0x40,
       "nothing but that row names the Help callback: the multiplayer main menu has no Help, "
       "and keeps every word")
    ok(not [a for a, _ in refs_into(elf, hdr["DL_HELP_CB"], hdr["DL_HELP_CB"] + 4)[0]],
       "and no code branches into the callback")
    ok(hooks[8][1] == hdr["MH_HELP_SCREEN_WORD"] == 0x38800000 | hdr["MH_SCREEN_ID"],
       "the callback's word becomes li r4,%d" % hdr["MH_SCREEN_ID"])

    # 4a. the game's own menus
    lo, hi = DL_START_OPTIONS
    inside = [h for h in hooks if lo <= h[0] < hi] + \
             [c for c in caves if c[0] < hi and c[0] + len(code if c[2] else data) > lo]
    ok(not inside and elf.word(DL_OPTIONS_SETBUTTONS[0]) == DL_OPTIONS_SETBUTTONS[1],
       "nothing goes into MenuMain_StartOptions (0x%08X..0x%08X): the Options list stays the "
       "game's, its call to SetMenuButtons at 0x%08X untouched" % (lo, hi, DL_OPTIONS_SETBUTTONS[0]))
    lo, hi = DL_MENU_PLAYER
    named = code_pointers(code, cave, lo, hi) + \
        ["data +0x%X: 0x%08X" % (i, struct.unpack(">I", data[i:i + 4])[0])
         for i in range(0, len(data) - 3, 4) if lo <= struct.unpack(">I", data[i:i + 4])[0] < hi]
    ok(not named, "nothing in the renderer names MenuMain_PlayerData (0x%08X..0x%08X), "
       "where Options keeps its rows" % (lo, hi))
    for x in named[:12]:
        print("        " + x)

    # 4b. the CONFIG page's game knowledge, against the Cheats screen's own code
    cw = [elf.word(0x00395C38 + 4 * i) for i in range(0, 0x700 // 4)]
    ok(0x38800000 | hdr["DL_TPL_SCREEN"] in cw and 0x38E00000 | hdr["DL_TPL_ROW"] in cw,
       "MenuCheats_CreateWidgets (0x395C38) makes its screen from template %d and its rows from %d" %
       (hdr["DL_TPL_SCREEN"], hdr["DL_TPL_ROW"]))
    plates = [v for a, v in refs_into(elf, 0x00ACA360, 0x00ACA3A0)[2] if 0x00396F04 <= a < 0x0039710C]
    ok(set(plates) >= {hdr["DL_PLATE_TOP"], hdr["DL_PLATE_BOTTOM"], hdr["DL_PLATE_FOOT"]},
       "its thread's Init (0x396F04) loads the three plates the shim loads")
    vt = [v for a, v in refs_into(elf, hdr["DL_CTLLIST_VTABLE"], hdr["DL_CTLLIST_VTABLE"] + 4)[2]
          if 0x00633618 <= a < 0x00633650]
    ok(vt == [hdr["DL_CTLLIST_VTABLE"]] and elf.word(0x0063363C) == 0x907F0000,
       "CtlList::CtlList (0x633618) writes the vtable 0x%08X at +0, as the shim does" % hdr["DL_CTLLIST_VTABLE"])

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
        if dat <= desc <= dat + len(data) - 8:
            c, t = struct.unpack(">II", data[desc - dat:desc - dat + 8])
            ok(cave <= c < cave + len(code) and dat + 0x8000 <= t <= dat + len(data) + 0x8000,
               "its %s 0x%08X is a descriptor {0x%08X in the code, TOC 0x%08X}" % (what, desc, c, t))
        else:
            ok(False, "its %s 0x%08X is in the data image" % (what, desc))

    # 4. the code, 5. the descriptors
    allowed = (hdr["GAME_HANDLE_MENU_INPUT"],)
    bad = lint_code(code, cave, allowed=allowed)
    ok(not bad, "the code: no branch out but the frame stub's tail branch, no sc, no vector instruction, "
       "no write to r13")
    for b in bad[:12]:
        print("        " + b)
    outs = [cave + i for i in range(0, len(code), 4)
            if branch_target(cave + i, struct.unpack(">I", code[i:i + 4])[0]) in allowed]
    ok(len(outs) == 1 and all((struct.unpack(">I", code[o - cave:o - cave + 4])[0] & 1) == 0 for o in outs),
       "exactly one way out, a plain branch (no link): the frame stub's tail call to HandleMenuInput")

    opds = dict((v, k) for k, v in hdr.items() if k.startswith("OPD_") and not k.endswith("#comment"))
    bad, seen = calls_via_descriptors(code, cave, set(opds), elf)
    ok(not bad, "every indirect call switches r2 through one of the header's descriptors and back")
    for b in bad[:12]:
        print("        " + b)
    ok(seen == set(opds), "and every descriptor the header names is one the code calls (%d of %d)" %
       (len(seen), len(opds)))
    for name in sorted(opds.values()):
        a = hdr[name]
        m = re.search(r"(0x[0-9A-Fa-f]+)\s*$", hdr.get(name + "#comment", ""))
        fn = int(m.group(1), 16) if m else None
        ok(fn is not None and elf.word(a) == fn and elf.word(a + 4) == DL_TOC,
           "%s 0x%08X = {0x%08X, TOC 0x%08X}" % (name, a, fn or 0, DL_TOC))

    # 8. the leaf stubs, stepped
    if "mh_inject_stub" in stub_at and "mh_screen_stub" in stub_at and "mh_help_label" in stub_at:
        try:
            check_stubs(ok, code, cave, hdr, box_defs, shim_defs, stub_at, data)
        except ValueError as e:
            ok(False, "the leaf stubs step: %s" % e)

    # 9. overlaps
    ours = [(cave, cave + len(code), "renderer code"), (mbx, mbx + len(box), "renderer mailbox"),
            (dat, dat + len(data), "renderer data")]
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
             mod_pointers(roots, "NPEA00423", mbx, mbx_end) +
             mod_pointers(roots, "NPEA00423", dat, dat_end)
             if not any(x.startswith("%s/%s " % (l, a)) for l, _ in roots for a in ACCEPTED)]
    ok(not named, "no mod's code names an address in either area")
    for x in named[:12]:
        print("        " + x)
    for c, mod in accepted:
        print("        known: %s: %s" % (c, ACCEPTED[mod]))


# ------------------------------------------------------------ Up Your Arsenal

UYA_TOC = 0x00C0A4E0

# qwark's own writes into UYA, build 54: src/games/rac3*.{c,h},
# src/games/sfhelper/sf_rac3.h, src/games/switches/sw_rac3.ld. (lo, hi, what).
QWARK_RAC3 = [
    (0x00980308, 0x009807F0, "the trophy reset's thread cave (savefile helper)"),
    (0x009807F0, 0x009808B0, "the fast-load cave"),
    (0x009808C0, 0x00980AC0, "the trophy reset's cave (savefile helper)"),
    (0x00980AC0, 0x00980F00, "the savefile helper's cave"),
    (0x00980F00, 0x00981064, "code switches (RPCS3)"),
    (0x0097CBA4, 0x0097CBA8, "savefile helper hook"),
    (0x00985318, 0x0098531C, "the trophy register's list fill, nopped"),
    (0x0027B49C, 0x0027B4A0, "fast loads, the pass site"),
    (0x0027B550, 0x0027B554, "fast loads, the draw site"),
    (0x00182A88, 0x00182A8C, "infinite ammo, the fingerprint"),
    (0x00D9FF00, 0x00D9FF50, "savefile helper bytes, trophy status, code switch flags"),
    (0x01100000, 0x01300000, "savefile aside buffer"),
]

# Overlaps that are by design: (mod folder) -> why.
ACCEPTED_RAC3 = {
    "probe-n3": "probe N3 is this renderer's first prototype and hooks the same label, combo and launcher "
                "words; the install refuses while it is loaded",
    "probe-n3b": "probe N3b is this renderer's prototype and hooks the same thirteen words but one; the "
                 "install refuses while it is loaded",
}

# The stubs' first words (mh_rac3_stubs.S), by the symbol in the hook's comment.
STUB_FIRST_RAC3 = {
    "mh_msg_stub": (0xFFFFFFFF, 0x54650036, "rlwinm r5,r3,0,0,27"),
    "mh_inject_stub": (0xFFFFFFFF, 0x54C60528, "rlwinm r6,r6,0,20,20, the game's own test"),
    "mh_combo_stub": (0xFFFFFFFF, 0x38A00001, "li r5,1"),
    "mh_launch_stub": (0xFFFFFFFF, 0xF821FF81, "stdu r1,-0x80(r1)"),
    "mh_rows8_stub": (0xFFFF0000, 0x3CC00000, "lis r6,..."),
    "mh_rows9a_stub": (0xFFFF0000, 0x3CC00000, "lis r6,..."),
    "mh_rows9b_stub": (0xFFFF0000, 0x3CC00000, "lis r6,..."),
}

# The main pause's three row tables: (address, rows, the HELP row's index).
UYA_TABLES = ((0x00C527C8, 8, 6), (0x00C52868, 9, 7), (0x00C5291C, 9, 7))


def check_core(ok, hdr, code, cave, core):
    """0: the core names no game address and calls nothing through a pointer."""
    cs, ce = core
    ok(cave <= cs <= ce <= cave + len(code), "the core is 0x%08X..0x%08X, %d bytes of the code" % (cs, ce, ce - cs))
    body = code[cs - cave:ce - cave]
    words = [struct.unpack(">I", body[i:i + 4])[0] for i in range(0, len(body) - 3, 4)]
    ok(0x4E800421 not in words,
       "the core calls no function through a pointer: it reaches the game only through the shim")
    page_lo, page_hi = (hdr["MH_BOX"] & 0xFFFF0000) - 0x10000, (hdr["MH_DATA_END"] + 0xFFFF) & 0xFFFF0000
    named = [x for x in code_pointers(body, cs, 0x00010000, 0xFFFFFFFF)
             if not page_lo <= int(x.split(": ")[1], 16) <= page_hi and
             (int(x.split(": ")[1], 16) >> 16) != (hdr["MH_MAGIC"] >> 16)]
    ok(not named, "and builds no address but its own areas'")
    for x in named[:12]:
        print("        " + x)


def check_rac3_stubs(ok, code, cave, hdr, box_defs, shim_defs, stub_at, data):
    shim, state, box = hdr["MH_SHIM"], hdr["MH_STATE"], hdr["MH_BOX"]
    pressed, force = shim + hdr["SH_PRESSED"], shim + hdr["SH_FORCE"]
    request = box + box_defs["MBOX_OFF_REQUEST"]
    opens = state + shim_defs["ST_OPEN_REQS"]
    text0 = state + shim_defs["ST_TEXT"]
    text_len = shim_defs["MH_TEXT_LEN"]
    game, planet = 0x01400000, hdr["U_PLANET"]

    def run(name, regs, mem=None):
        cpu = Ppc(code, cave, {})
        for (a, n), v in (mem or {}).items():
            cpu.st(a, v, n)
        for k, v in regs.items():
            cpu.r[k] = v
        return cpu, cpu.run(stub_at[name])

    def cstr(a):
        if not hdr["MH_DATA"] <= a < hdr["MH_DATA"] + len(data):
            return None
        return data[a - hdr["MH_DATA"]:].split(b"\0")[0].decode("latin1")

    # The label hook: RACMAN, the rows, and everything else on into msg_string.
    msg_ok = {"r3", "r4", "r5", "cr0"}
    cpu, how = run("mh_msg_stub", {3: hdr["MH_MSG_HELP"], 4: 0x1234})
    ok(how == ("blr", None) and cstr(cpu.r[3]) == "RACMAN" and cpu.written <= msg_ok,
       "msg stub, 0xF000: \"RACMAN\", in the data image, back to msg_string's caller")
    good = True
    for i in range(hdr["MH_MAIN_ROWS"]):
        cpu, how = run("mh_msg_stub", {3: hdr["MH_MSG_ROW0"] + i})
        good = good and how == ("blr", None) and cpu.r[3] == text0 + i * text_len and cpu.written <= msg_ok
    ok(good, "msg stub, 0xF001 + i for each of the %d MAIN rows: the core's row i (mh_text(i))" % hdr["MH_MAIN_ROWS"])
    for idn in (hdr["MH_MSG_ROW0"] + hdr["MH_MAIN_ROWS"], 0xF00F, 0xF010, 0xEFFF, 0x106, 0x522):
        cpu, how = run("mh_msg_stub", {3: idn, 4: 0x1234})
        ok(how == ("b", hdr["GAME_MSG_EXIT"]) and cpu.r[3] == idn and cpu.r[4] == 0 and cpu.written <= msg_ok,
           "msg stub, 0x%X: on into msg_string with r4 = 0 (its first instruction), r3 kept" % idn)

    # The inject: Start for this test in the frame that takes an open request.
    inject_ok = {"r0", "r6", "r11", "r12", "cr0"}

    def inject(req, edges=0x40, mp=0, plan=5, before=1):
        mem = {(request, 1): req, (pressed, 1): before, (hdr["U_GAME_PTR"], 4): game,
               (game + hdr["U_GAME_MP"], 1): mp, (planet, 4): plan, (opens, 4): 5}
        return run("mh_inject_stub", {3: 0x333, 4: 0x444, 5: 0x555, 6: edges}, mem)

    cpu, how = inject(0)
    ok(how == ("blr", None) and cpu.r[6] == 0 and cpu.ld(pressed, 1) == 0 and cpu.written <= inject_ok,
       "inject, no request, no Start: r6 0, PRESSED down")
    cpu, how = inject(0, edges=0x840)
    ok(cpu.r[6] == 0x800 and cpu.ld(pressed, 1) == 0, "inject, the player's Start: the game's own test, PRESSED down")
    cpu, how = inject(1)
    ok(how == ("blr", None) and cpu.r[6] == 0x800 and cpu.ld(request, 1) == 0 and cpu.ld(pressed, 1) == 1 and
       cpu.ld(opens, 4) == 6 and cpu.r[3] == 0x333 and cpu.r[4] == 0x444 and cpu.r[5] == 0x555 and
       cpu.written <= inject_ok,
       "inject, an open request: taken, the test answers Start, PRESSED up, counted, r3 to r5 kept")
    cpu, how = inject(1, mp=1)
    ok(cpu.r[6] == 0 and cpu.ld(request, 1) == 0 and cpu.ld(pressed, 1) == 0,
       "inject in multiplayer: the request taken, no Start")
    for plan in (30, 33, 36):
        cpu, how = inject(1, plan=plan)
        ok(cpu.r[6] == 0 and cpu.ld(request, 1) == 0 and cpu.ld(pressed, 1) == 0,
           "inject on planet %d, a vid comic: the request taken, no Start" % plan)
    for plan in (29, 37, 1):
        cpu, how = inject(1, plan=plan)
        ok(cpu.r[6] == 0x800, "inject on planet %d: Start" % plan)
    cpu, how = inject(2)
    ok(cpu.r[6] == 0 and cpu.ld(request, 1) == 0 and cpu.ld(pressed, 1) == 0,
       "inject, a close request: dropped, nothing to close in play")

    # The Start path tells the launcher.
    for p in (0, 1):
        cpu, how = run("mh_combo_stub", {3: 3, 4: 1, 6: 0x66}, {(pressed, 1): p, (force, 1): 1 - p})
        ok(how == ("blr", None) and cpu.r[5] == 1 and cpu.ld(force, 1) == p and cpu.r[3] == 3 and
           cpu.r[4] == 1 and cpu.r[6] == 0x66 and cpu.written <= {"r5", "r11", "r12"},
           "combo stub, PRESSED %d: r5 = 1, FORCE %d" % (p, p))

    # The rows stubs.
    table, count = shim + hdr["SH_LIST_TABLE"], shim + hdr["SH_LIST_COUNT"]
    band = shim + hdr["SH_BAND"]
    rows_ok = {"r3", "r6", "r7", "cr0"}
    for name, n, gtable, off in (("mh_rows8_stub", 8, 0x00C527C8, hdr["MH_BAND_OFF8"]),
                                 ("mh_rows9a_stub", 9, 0x00C52868, hdr["MH_BAND_OFF9A"]),
                                 ("mh_rows9b_stub", 9, 0x00C5291C, hdr["MH_BAND_OFF9B"])):
        regs = {3: gtable, 4: 0x00C50000, 5: 0x00C50000, 7: 0x00C50000, 8: 0x00C50000}
        cpu, how = run(name, regs)
        ok(how == ("blr", None) and cpu.r[6] == n and cpu.r[3] == gtable and cpu.r[7] == 0x00C50000 and
           cpu.r[4] == cpu.r[5] == 0x00C50000 and cpu.written <= rows_ok,
           "%s, idle: r6 = %d, the game's table and band" % (name, n))
        cpu, how = run(name, regs, {(table, 4): shim + hdr["SH_ROWS"], (count, 4): 3})
        ok(how == ("blr", None) and cpu.r[3] == shim + hdr["SH_ROWS"] and cpu.r[6] == 3 and
           (cpu.r[7] + off) & 0xFFFFFFFF == band and cpu.r[4] == 0x00C50000 and cpu.written <= rows_ok,
           "%s, the shim's init running: its table, its count, and r7 + 0x%X = its band" % (name, off))


def check_rac3(ok, elf, roots):
    hdr = header_defs(os.path.join(HERE, "mh_rac3.h"))
    box_defs = header_defs(MENUBOX)
    shim_defs = header_defs(SHIM_H)
    shim_defs["ST_TEXT"] = shim_text_offset(hdr["MH_ROWS"])
    box, data, code, hooks, caves, core = parse_bins(GEN_C, "rac3")
    print("\nNPEA00387 console menu renderer")
    if box is None or data is None or code is None or core is None:
        ok(False, "menuhelper_bins.c has rac3's mailbox, data, code and the core's place in it")
        return

    cave, cave_end = hdr["MH_CAVE"], hdr["MH_CAVE_END"]
    mbx, mbx_end = hdr["MH_BOX"], hdr["MH_BOX_END"]
    dat, dat_end = hdr["MH_DATA"], hdr["MH_DATA_END"]
    tv, tfs, tms, _, _ = elf.text
    rv, rfs, rms, _, _ = elf.rw

    ok(len(caves) == 4 and [c[:3] for c in caves] ==
       [(mbx, "mh_rac3_box", 0), (dat, "mh_rac3_state", 0), (caves[2][0], "mh_rac3_data", 0),
        (cave, "mh_rac3_code", 1)] and caves[2][3] == 0 and caves[3][3] == 0,
       "four caves: the mailbox at 0x%08X, the state at 0x%08X, the read-only data, then the code at 0x%08X" %
       (mbx, dat, cave))
    rest = parse_rest(GEN_C, "rac3")
    ok(caves[0][3] > 0 and caves[1][3] > 0 and caves[2][0] == dat + len(data) - len(rest),
       "the mailbox and the state are a few bytes and then zeros, %d and %d of them, carried as counts; "
       "the read-only data follows the state" % (caves[0][3], caves[1][3]))
    ok(cave + len(code) <= cave_end and mbx + len(box) <= mbx_end and dat + len(data) <= dat_end,
       "code %d bytes (of %d), mailbox %d bytes (of %d), data %d bytes (of %d)" %
       (len(code), cave_end - cave, len(box), mbx_end - mbx, len(data), dat_end - dat))

    # 0. the core
    check_core(ok, hdr, code, cave, core)

    # 6. the areas
    ok(cave >= tv + tms and cave_end <= page_end(tv + tms),
       "code area 0x%08X..0x%08X is past the text segment's end (0x%08X), in its last page" %
       (cave, cave_end, tv + tms))
    for lo, hi, what in ((mbx, mbx_end, "mailbox"), (dat, dat_end, "data")):
        ok(lo >= rv + rms and hi <= page_end(rv + rms),
           "%s area 0x%08X..0x%08X is past the read-write segment's end (0x%08X), in its last page" %
           (what, lo, hi, rv + rms))
    for lo, hi, what in ((cave, cave_end, "code"), (mbx, mbx_end, "mailbox"), (dat, dat_end, "data")):
        br, wd, pr = refs_into(elf, lo, hi)
        ok(not wd and not pr, "no word and no lis/addi pair in the executable names the %s area%s" %
           (what, "" if not br else " (%d data words decode as branches into it)" % len(br)))

    # 7. the mailbox and the data image
    magic, version, size = struct.unpack(">IHH", box[0:8])
    ok(len(box) == hdr["MH_MBOX_SIZE"] and magic == box_defs["MBOX_MAGIC"] and
       version == box_defs["MBOX_VERSION"] and size == hdr["MH_MBOX_SIZE"],
       "the mailbox image: 'QMNU', version %d, %d bytes" % (version, size))
    ok(box[8] == hdr["MH_ROWS"] == 16 and box[9] == hdr["MH_ROW_BYTES"] == box_defs["MBOX_ROW_SIZE"] and
       box[10] == hdr["MH_MAIN_ROWS"] == 9 and box[11] == hdr["MH_CAPS"] and box[12] == 0 and box[13] == 0 and
       box[14] == 3 and box[15] == hdr["MH_CFG_ROWS"] == 16,
       "row_cap 16, rows of 0x40 (no description line), 9 rows a MAIN page, 16 a CONFIG page, caps 0x%02X, "
       "closed, no request, renderer 3" % box[11])
    ok(box_defs["MBOX_OFF_ROWS"] + hdr["MH_ROWS"] * hdr["MH_ROW_BYTES"] == hdr["MH_MBOX_SIZE"] and
       all(b == 0 for b in box[16:]), "the rows fill the image, everything after the header zero")
    shim_off = hdr["MH_SHIM"] - hdr["MH_STATE"]
    ok(struct.unpack(">I", data[0:4])[0] == hdr["MH_MAGIC"] and
       all(b == 0 for b in data[4:shim_off + hdr["SH_SIZE"]]),
       "the data image: the core's state at MH_STATE with its magic 'QMH3', the shim's at MH_SHIM "
       "(+0x%X), everything else zero" % shim_off)
    ok(shim_off + hdr["SH_SIZE"] <= len(data), "the shim's state, %d bytes, is all in the image" % hdr["SH_SIZE"])
    cells = hdr["MH_SHIM"] + hdr["SH_CELLS"]
    ok(hdr["MH_CFG_ROWS"] * hdr["U_GRID_COLS"] * hdr["U_CELL_SIZE"] < hdr["SH_CELLS_SIZE"] and
       hdr["SH_CELLS"] + hdr["SH_CELLS_SIZE"] <= hdr["SH_SIZE"] and cells % 4 == 0,
       "the grid's cells, %d bytes at 0x%08X: more than a page of 16 rows of two cells of 68 (GridBuild "
       "builds nothing in a buffer that is not)" % (hdr["SH_CELLS_SIZE"], cells))

    # 1. the originals, 2. the hook words
    order = ["MH_SITE_MSG", "MH_SITE_ROWS8", "MH_SITE_ROWS9A", "MH_SITE_ROWS9B", "MH_SITE_INJECT",
             "MH_SITE_COMBO", "MH_SITE_LAUNCH", "MH_SITE_HELP8_TEXT", "MH_SITE_HELP8_CB",
             "MH_SITE_HELP9A_TEXT", "MH_SITE_HELP9A_CB", "MH_SITE_HELP9B_TEXT", "MH_SITE_HELP9B_CB"]
    ok(len(hooks) == 13 and [h[0] for h in hooks] == [hdr[k] for k in order],
       "thirteen words: the label hook, the three row counts, the Start path's three, then the HELP rows' six")
    for a, value, orig, what in hooks:
        ok(elf.word(a) == orig, "0x%08X holds 0x%08X in the executable (%s)" % (a, orig, what))

    stub_at, help_desc = {}, None
    for a, value, orig, what in hooks:
        m = re.match(r"(b|bl) (mh_\w+_stub)", what)
        if m:
            link = m.group(1) == "bl"
            t = branch_target(a, value) if (value >> 26) == 18 and (value & 3) == (1 if link else 0) else None
            first = struct.unpack(">I", code[t - cave:t - cave + 4])[0] \
                if t is not None and cave <= t < cave + len(code) else None
            mask, want, name = STUB_FIRST_RAC3[m.group(2)]
            good = first is not None and (first & mask) == want
            ok(good, "0x%08X: %08X = %s 0x%08X, %s (%s)" % (a, value, m.group(1), t or 0, m.group(2), name))
            if good:
                stub_at[m.group(2)] = t
        elif "text" in what:
            ok(value == hdr["MH_MSG_HELP"], "0x%08X: the HELP row's text id 0x%X becomes 0x%X, \"RACMAN\"" %
               (a, orig, value))
        elif "callback" in what:
            good = dat <= value <= dat + len(data) - 8
            if good:
                c, t = struct.unpack(">II", data[value - dat:value - dat + 8])
                good = cave <= c < cave + len(code) and dat + 0x8000 <= t <= dat + len(data) + 0x8000
                help_desc = value if good else None
            ok(good, "0x%08X: the HELP row's callback 0x%08X becomes 0x%08X, a descriptor {code in the "
               "code image, the renderer's TOC}" % (a, orig, value))

    # The sites in the game's code, and around them.
    W = elf.word
    ctx = [
        (0x00BD7518, 0x0014BCF0, "msg_string's descriptor names 0x14BCF0, the label hook's site"),
        (0x0014BCF4, 0x2C830508, "cmpwi cr1,r3,1288 after it, where the stub goes on"),
        (0x0028717C, 0x80C6D574, "the mode 0 update loads the pad pointer 0xC1D574 ..."),
        (0x00287180, 0x80C600A4, "... and its presses, pad +0xA4, before the inject site"),
        (0x00287188, 0x2C060000, "cmpwi r6,0 after it"),
        (0x002871A4, 0x80C69330, "then the gate: the frame count 0xEE9330 ..."),
        (0x0028725C, 0x38600003, "the Start path: li r3,3"),
        (0x00287260, 0x38800001, "li r4,1"),
        (0x00287268, 0x38C00000, "li r6,0 after the combo site"),
        (0x0028726C, 0x38E00000, "li r7,0"),
        (0x00287270, 0x4BFA2329, "bl 0x229598, mode_requestChange(3, 1, 1, 0, 0)"),
        (0x00287244, 0x38A00005, "the vid comics' pause is reason 5, a path of its own"),
        (0x002D4448, 0x60000000, "the launcher: nop after the getter call"),
        (0x002D444C, 0x4BFDB371, "bl 0x2AF7BC, SwapTo(r3)"),
        (0x00BDE3D8, 0x002F3C18, "the main pause getter's descriptor names the call's target"),
        (0x002F4278, 0x306327C8, "8 rows: r3 = 0xC527C8"),
        (0x002F4280, 0x3CE000C5, "lis r7,0xC5"),
        (0x002F428C, 0x906429E8, "after: stw r3, the table"),
        (0x002F4290, 0x306729D0, "addic r3,r7,10704, the band 0xC529D0"),
        (0x002F4294, 0x90C529F4, "stw r6, the count"),
        (0x002F41F0, 0x30632868, "9 rows: r3 = 0xC52868"),
        (0x002F41F8, 0x3CE000C5, "lis r7,0xC5"),
        (0x002F4208, 0x306729D8, "addic r3,r7,10712, the band 0xC529D8"),
        (0x002F420C, 0x90C529F4, "stw r6, the count"),
        (0x002F4248, 0x3063291C, "9 rows: r3 = 0xC5291C"),
        (0x002F424C, 0x3CE000C5, "lis r7,0xC5"),
        (0x002F4258, 0x30E729E0, "after: addic r7,r7,10720, the band 0xC529E0"),
        (0x002F4260, 0x90C529F4, "stw r6, the count"),
        (0x002F4754, 0x80BF29EC, "the init reads the band: the end pieces' heights"),
        (0x00C529D0, 0x3E23D70A, "8 rows' band: 0.16 ..."),
        (0x00C529D4, 0x3F59999A, "... and 0.85, which the shim's 0.40 - 0.03n and 0.61 + 0.03n give"),
        (0x00C529D8, 0x3E051EB8, "9 rows' band: 0.13 ..."),
        (0x00C529DC, 0x3F6147AE, "... and 0.88"),
        (0x002F4BF4, 0x806329F0, "the main pause's update on the cross: the cursor 0xC529F0"),
        (0x002F4C1C, 0x8063000C, "the row's callback"),
        (0x002F4C64, 0x88630010, "then its ResetState byte"),
        (0x002F4C94, 0x546306F6, "Triangle (0x10): out of the pause, which the shim takes first"),
        (0x00BDE430, 0x002F3FEC, "HELP's callback descriptor names 0x2F3FEC ..."),
        (0x002F3FF8, 0x4BFF67D9, "... which swaps to HELP (bl 0x2EA7D0, the getter)"),
        (0x00BDE458, 0x002F4EA8, "the main pause's pre-render ..."),
        (0x002F4EB4, 0x4BFDDB01, "... calls the shared pre-render 0x2D29B4"),
        (0x002F4ED8, 0x4BFDDB69, "and its post-render the shared 0x2D2A40"),
        (0x002DCB34, 0x38800000, "the grid screens' alignment: column 0 ..."),
        (0x002DCB38, 0x38A00001, "x 1"),
        (0x002DCB3C, 0x38C00003, "y 3"),
        (0x002DCB58, 0x38800001, "column 1 ..."),
        (0x002DCB5C, 0x38A00002, "x 2, as the shim aligns its two"),
        (0x002E2BC8, 0x38A00001, "CHEATS colours a locked cheat 1 (grey) ..."),
        (0x002D6ABC, 0x2C0A0004, "and the grid's Up skips a row of colour 4 ..."),
        (0x002D6BD0, 0x2C0C0004, "and its Down"),
        (0x002D6EF4, 0x80850090, "colour 4 draws in the palette's grey, +0x90 (as 1)"),
    ]
    for a, want, what in ctx:
        if what is None:
            continue
        ok(W(a) == want, "0x%08X holds 0x%08X: %s" % (a, want, what))
    ok(branch_target(hdr["MH_SITE_LAUNCH"], hdr["MH_ORIG_LAUNCH"]) == 0x002F3C18,
       "the launch site calls the main pause's getter, 0x2F3C18")
    ok(branch_target(0x0028718C, W(0x0028718C)) == 0x002871A0,
       "and the Start test branches to the Start path (0x2871A0) when r6 is not 0")
    ok(branch_target(0x0028722C, W(0x0028722C)) == 0x0014AD88 and W(0x0014AD90) == 0x2C840024 and
       W(0x0014AD94) == 0x2C04001E,
       "the Start path asks 0x14AD88 for planets 30..36, the vid comics, which the inject stub leaves alone")

    # 4a. the game's own menus: the HELP rows' two words, and nothing else of the tables
    for tab, n, help_row in UYA_TABLES:
        row = tab + 20 * help_row
        texts = [W(tab + 20 * i + 4) for i in range(n)]
        ok(W(row) in (0x000D0012, 0x000D0013) and W(row + 4) == hdr["MH_ORIG_HELP_TEXT"] and
           W(row + 8) == 0x01000000 and W(row + 12) == hdr["MH_ORIG_HELP_CB"] and W(row + 16) == 0x01000000 and
           texts.count(hdr["MH_ORIG_HELP_TEXT"]) == 1,
           "table 0x%08X: row %d is HELP (text 0x106, enabled, callback 0xBDE430, reset after), its only one" %
           (tab, help_row))
        inside = [h[0] for h in hooks if tab <= h[0] < tab + 20 * n]
        ok(inside == [row + 4, row + 12],
           "and the renderer writes nothing else of it: its other rows and their order stay the game's")
    br, wd, pr = refs_into(elf, hdr["MH_ORIG_HELP_CB"], hdr["MH_ORIG_HELP_CB"] + 8)
    ok(sorted(a for a, _ in wd) == sorted(hdr[k] for k in ("MH_SITE_HELP8_CB", "MH_SITE_HELP9A_CB",
                                                           "MH_SITE_HELP9B_CB")) and not pr,
       "nothing but the three HELP rows names HELP's callback descriptor")

    # 4b. the grid configuration the shim builds on: CHEATS'
    game_cfg = [W(0x00C506EC + 4 * i) for i in range(24)]
    m = re.search(r"static const u32 cfg_template\[24\] = \{(.*?)\};",
                  open(os.path.join(HERE, "mh_rac3.c"), encoding="utf-8").read(), re.S)
    tpl = []
    if m:
        for tok in m.group(1).replace("\n", " ").split(","):
            tok = tok.strip()
            if tok:
                tpl.append(hdr[tok] if tok in hdr else int(tok, 0))
    ok(len(tpl) == 24 and all(tpl[i] == game_cfg[i] for i in range(24) if i not in (0, 1, 3, 4, 7, 8)) and
       tpl[2] == 0x14 and tpl[5] == 2 and tpl[7] == 0x3EE66666 and tpl[8] == 0x3D591687,
       "the shim's grid configuration is CHEATS' (0xC506EC) but for the buffer, its size, the rows and the "
       "two floats CHEATS writes on first use (0x2E27E4)")

    # 3. the descriptors the game calls
    if help_desc is not None:
        c = struct.unpack(">I", data[help_desc - dat:help_desc - dat + 4])[0]
        ok(c != 0, "mh_help is code at 0x%08X" % c)

    # 4. the code, 5. the descriptors
    allowed = (hdr["GAME_MSG_EXIT"],)
    bad = lint_code(code, cave, allowed=allowed)
    ok(not bad, "the code: no branch out but the label stub's, no sc, no vector instruction, no write to r13")
    for b in bad[:12]:
        print("        " + b)
    outs = [cave + i for i in range(0, len(code), 4)
            if branch_target(cave + i, struct.unpack(">I", code[i:i + 4])[0]) in allowed]
    ok(len(outs) == 1 and all((struct.unpack(">I", code[o - cave:o - cave + 4])[0] & 3) == 0 for o in outs),
       "exactly one way out, a plain branch: the label stub on into msg_string")
    launch = stub_at.get("mh_launch_stub")
    if launch is not None:
        lw = [struct.unpack(">I", code[launch - cave + 4 * i:launch - cave + 4 * i + 4])[0] for i in range(13)]
        t = branch_target(launch + 24, lw[6])
        ok(lw[1] == 0x7C0802A6 and lw[2] == 0xF8010090 and lw[3] == 0xF8410078 and (lw[6] >> 26) == 18 and
           (lw[6] & 3) == 1 and cave <= t < cave + len(code) and lw[8] == 0xE8410078 and lw[12] == 0x4E800020,
           "the launch stub saves LR and the game's r2, calls mh_launch with the renderer's TOC, puts both "
           "back and returns to the launcher")

    opds = dict((v, k) for k, v in hdr.items() if k.startswith("OPD_") and not k.endswith("#comment"))
    handed = {hdr["U_DESC_PRE"], hdr["U_DESC_POST"]}
    bad, seen = calls_via_descriptors(code, cave, set(opds) | handed, elf, toc=UYA_TOC, code_hi=0xBC0000)
    ok(not bad, "every indirect call switches r2 through one of the header's descriptors and back")
    for b in bad[:12]:
        print("        " + b)
    ok(seen - handed == set(opds), "and every descriptor the header names is one the code calls (%d of %d)" %
       (len(seen - handed), len(opds)))
    for name in sorted(opds.values()):
        a = hdr[name]
        m = re.search(r"(0x[0-9A-Fa-f]+)\s*$", hdr.get(name + "#comment", ""))
        fn = int(m.group(1), 16) if m else None
        ok(fn is not None and elf.word(a) == fn and elf.word(a + 4) == UYA_TOC,
           "%s 0x%08X = {0x%08X, TOC 0x%08X}" % (name, a, fn or 0, UYA_TOC))
    for name, fn in (("U_DESC_PRE", 0x002D29B4), ("U_DESC_POST", 0x002D2A40)):
        ok(elf.word(hdr[name]) == fn and elf.word(hdr[name] + 4) == UYA_TOC,
           "%s 0x%08X = {0x%08X, TOC}, the render the shim's GuiStates are handed" % (name, hdr[name], fn))
    named = code_pointers(code, cave, hdr["U_DESC_PRE"], hdr["U_DESC_PRE"] + 8) + \
        code_pointers(code, cave, hdr["U_DESC_POST"], hdr["U_DESC_POST"] + 8)
    ok(len(named) >= 2, "and the code hands both to its GuiStates")

    # 8. the leaf stubs, stepped
    if all(k in stub_at for k in ("mh_msg_stub", "mh_inject_stub", "mh_combo_stub", "mh_rows8_stub",
                                  "mh_rows9a_stub", "mh_rows9b_stub")):
        try:
            check_rac3_stubs(ok, code, cave, hdr, box_defs, shim_defs, stub_at, data)
        except ValueError as e:
            ok(False, "the leaf stubs step: %s" % e)
    else:
        ok(False, "every leaf stub found to step")

    # 9. overlaps
    ours = [(cave, cave + len(code), "renderer code"), (mbx, mbx + len(box), "renderer mailbox"),
            (dat, dat + len(data), "renderer data")]
    ours += [(a, a + 4, "renderer word") for a, _, _, _ in hooks]
    known = mod_entries(roots, "NPEA00387")
    clashes, accepted = [], []
    for lo, hi, what in ours:
        for a, b, label, mod, kind in known:
            if a < hi and b > lo:
                text = "%s 0x%08X..0x%08X vs %s/%s %s 0x%08X..0x%08X" % (what, lo, hi, label, mod, kind, a, b)
                (accepted if mod in ACCEPTED_RAC3 and what == "renderer word" else clashes).append((text, mod))
        for a, b, w in QWARK_RAC3:
            if a < hi and b > lo:
                clashes.append(("%s 0x%08X..0x%08X vs qwark %s 0x%08X..0x%08X" % (what, lo, hi, w, a, b), None))
    ok(not clashes, "no overlap with %d mod words and caves (the probes' among them), nor with qwark's %d own "
       "(the trophy caves and the fast-load cave at 0x9807F0..0x9808B0 among them)" % (len(known), len(QWARK_RAC3)))
    for c, _ in clashes:
        print("        " + c)
    named = [x for x in mod_pointers(roots, "NPEA00387", cave, cave_end) +
             mod_pointers(roots, "NPEA00387", mbx, mbx_end) +
             mod_pointers(roots, "NPEA00387", dat, dat_end)]
    ok(not named, "no mod's code names an address in any of the three areas")
    for x in named[:12]:
        print("        " + x)
    seen_acc = set()
    for c, mod in accepted:
        if mod not in seen_acc:
            print("        known: %s/...: %s" % (mod, ACCEPTED_RAC3[mod]))
            seen_acc.add(mod)
    print("        (%d words shared with the prototypes by design)" % len(accepted))


def shim_text_offset(rows):
    """ST_TEXT for a renderer of `rows` mailbox rows (mh_shim.h)."""
    st_kind = 0x38
    st_flags = st_kind + rows
    st_id = (st_flags + rows + 1) & ~1
    st_title = (st_id + 2 * rows + 3) & ~3
    return st_title + 0x24 + 0x44


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
        # The hardware probes as built, whether or not the user has them installed.
        probes = find_up(os.path.join("console-ui-probes", "a-ingame", "out"))
        if probes:
            roots.append(("probes", probes))
    roots = [(l, p) for l, p in roots if os.path.isdir(p)]

    failures = 0

    def ok(cond, msg):
        nonlocal failures
        print(("  ok    " if cond else "  FAIL  ") + msg)
        if not cond:
            failures += 1

    print("mod libraries: " + (", ".join("%s (%s)" % (l, p) for l, p in roots) or "none"))
    check_rac4(ok, Elf(os.path.join(eboots, "NPEA00423.elf")), roots)
    check_rac3(ok, Elf(os.path.join(eboots, "NPEA00387.elf")), roots)

    print("\n%s" % ("all checks passed" if failures == 0 else "%d check(s) FAILED" % failures))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
