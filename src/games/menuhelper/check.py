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
  4b. what the shim takes from the game is what GAME SETTINGS uses (round 3):
     its screen template, title, legend and frame elements, its plates, its
     row table's widgets (the ones the shim hides) and its sounds, the open
     sound's two bytes, the Cheats screen's row template and CtlList's
     vtable; and, when the PS3 build's bonus.ps3 is at hand (the rpcs3
     install, or --bonus), templates 37 and 28 themselves: every element the
     shim names is the widget it takes it for, and the slider's geometry is
     GAME SETTINGS';
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

For Up Your Arsenal's renderer, the same, and for its fourteen words: each
holds the game's word in the executable, the code words are branches to the
stubs their comments name, the HELP rows' words are the text id 0xF000 and a
descriptor of the renderer's; the code around each site is what the stubs
assume (the msg_string entry, the main pause init's three tables and bands,
the Start test and path, the launcher, the GALACTIC MAP's list build); the
GALACTIC MAP the Planets row opens is what the shim takes it for (its
GuiState's five descriptors, the list, entries and cursor its update reads,
the exits the shim takes first, the picture of every planet of the Trainer's
list, the five name words and a name apart for each entry, the Trainer's list
rac3.c's menu order); the renderer writes nothing of the
main pause's tables but the HELP rows' two words; what the CONFIG page takes
from Options' AUDIO / VISUAL screen is that screen's (every AV_* number where
the screen reads it, the grid configuration it builds and the calls after it,
the panel of its widget table, the sliders' sizes and colours, font 1, the
text fit, the grid's fields the sliders follow, and handles no other code
builds; round 4: the picture, its source, buffers and set as AUDIO / VISUAL's
init, update and deinit use them); the grid's cells' span (round 4) is past
the read-write segment's end and named by nothing; the label, inject, combo,
rows and map list stubs, stepped, do what the C and the tests assume; the code
span, grown to N3's start, is named by nothing and overlaps no mod; and nothing
overlaps the mods, the probes or qwark's own writes (the trophy caves and the
fast-load cave at 0x9807F0..0x9808B0), but for probes N3 and N3b, the
prototypes, whose words are the same by design.

For Going Commando's renderer, the same again, and for its six words: the
label hook, the Start test, the pause reason and the root the pause manager
opens on, each a branch to the stub its comment names, and the root's Help
entry's screen and label, a screen of the renderer's and the text id 0xF000;
the code round each site is what the stubs assume (the msg_string entry, the
Start test and path, the manager's state 1 and its screen changes); the six
screens in the data image are copies of the root's and Cheats', with the
renderer's item; Cheats' draw has the layout the shim copies; the root's setup
asks the six questions the inject asks; the label, inject, combo and root stubs,
stepped, do what the C and the tests assume; and nothing overlaps the mods, the
probes or qwark's own writes, but for probes N2 to N2c, whose words are the
same by design.
For Ratchet & Clank's renderer, the same again for its four words and its
four code areas, which are runs of code nothing reaches rather than a tail
past the text segment: each runs from a function's start to the next's,
nothing outside it branches into it, no lis/addi pair names it, and the only
words naming it are its own functions' descriptors, which nothing names; the
code round the sites (get_ui_string's
not-found path, the play update's call), the main page's Help item and entry,
Goodies > Cheats' page, title, list and hint, In-Level Movies' title pose and
Cinematics' list in the same pane, the constants of Cheats' list draw and of
Camera Options' 3D slider that the shim's draw follows; the planet picker's
Galactic Map: its page, title, list, galaxy, hint and picture, the planet table,
the transition's rule the picker's moves use and the close action screen 50
keeps plain; the text stub, stepped, and the play stub's
frame; and no overlap with the mods (charge-boots among them), the probes or
qwark's own writes (the savefile helper and the code switches), but for
probes N1, N1b and N1c, whose words are the same by design.

For every renderer (revision 1.23, build 55, menu.help): the Help row's words
are the hooks at the header's MH_SITE_HELP* sites, every one of them, and they
are the table's last, as many as the generated table's `nhelp` says, so menu.c
can put them in and take them out alone and in order; and no code of the
renderer names a Help site and no word of its data image holds one, so it never
reads them and draws, opens and closes the same whether they are in or out.
Each Help word's game word is what the install restores at menu.help = 0, and
check 1 above holds the executables to it.
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
    "probe-pl4": "probe PL4 is the planet picker's prototype and hooks Worlds' X and its stop the same way; "
                 "the install refuses while it is loaded",
    "probe-pl4-list": "probe PL4's list mod writes the four words the picker gates, as plain words; the install "
                      "refuses while it is loaded",
    "probe-n0": "probe N0 is this renderer's prototype and hooks the same sites; "
                "the install refuses while it is loaded, and N0 never needs loading again",
}

# The stubs' first words (mh_rac4_stubs.S).
STUB_FIRST = {
    "mh_select_stub": (0xFFFFFFFF, 0xF821FF81, "stdu r1,-0x80(r1)"),
    "mh_gate_ghost": (0xFFFF0003, 0x40820000, "bne, the game's own test first"),
    "mh_gate_count": (0xFFFFFFFF, 0x3860000F, "li r3,15, the game's own count first"),
    "mh_gate_back": (0xFFFF0003, 0x40820000, "bne, the game's own test first"),
    "mh_gate_close": (0xFFFF0003, 0x40820000, "bne, the game's own test first"),
    "mh_gate_locked": (0xFFFF0000, 0x3D800000, "lis r12,..., the picker's byte"),
    "mh_gate_skills": (0xFFFF0003, 0x40820000, "bne, the game's own test first"),
    "mh_gate_tourney": (0xFFFF0003, 0x41820000, "beq, a planet on its way before anything is touched"),
    "mh_gate_legend": (0xFFFF0000, 0x3D800000, "lis r12,..., the picker's byte"),
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
            continue
        if op in (14, 24, 32, 34, 36, 38, 40, 44, 48, 52, 58, 62):
            ra = (w >> 21) & 31 if op == 24 else (w >> 16) & 31
            if ra in lis:
                imm = w & 0xFFFF
                v = ((lis[ra] << 16) | imm) if op == 24 else \
                    (lis[ra] << 16) + (imm - 0x10000 if imm & 0x8000 else imm)
                if lo <= (v & 0xFFFFFFFF) < hi:
                    found.append("0x%08X: 0x%08X" % (base + i, v & 0xFFFFFFFF))
        # A register written by anything but an addis holds no high half any
        # more (round 3: a set's address made as `addis rD,rIndex,hi` and then
        # used for a sum read as a pair with an addi much further on).
        for d in written_regs(w):
            lis.pop(d, None)
    return found


def written_regs(w):
    """The general registers an instruction writes, as far as the pairs above care."""
    op = w >> 26
    rd, ra = (w >> 21) & 31, (w >> 16) & 31
    if op in (7, 8, 12, 13, 14, 32, 33, 34, 35, 40, 41, 42, 43, 58):
        return (rd,)
    if op in (20, 21, 23, 24, 25, 26, 27, 28, 29, 30):
        return (ra,)
    if op == 31:
        xo = (w >> 1) & 0x3FF
        if xo in (28, 60, 124, 284, 316, 412, 444, 476, 24, 27, 536, 539, 792, 794, 824, 826, 827,
                  954, 922, 986, 26, 58):
            return (ra,)
        if xo & 0x1FF in (266, 10, 138, 234, 202, 40, 8, 136, 232, 200, 104, 235, 233, 75, 11, 73, 9,
                          491, 459, 489, 457) or xo in (23, 55, 87, 119, 279, 311, 343, 375, 21, 53,
                                                        339, 371, 19, 83):
            return (rd,)
        if xo == 467 or xo == 144:
            return ()
    return ()


def uses_reg(w, reg):
    """Whether an instruction names general register `reg` in a register field (branches, CR ops and a
    compare's CR field apart): what a gate that writes `reg` must not find in the function round it."""
    op = w >> 26
    rd, ra, rb = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    if op in (16, 17, 18, 19):
        return False
    if op in (10, 11):
        return ra == reg
    if op == 31:
        return reg in (rd, ra, rb)
    if op in (21, 23):
        return reg in (rd, ra) or (op == 23 and rb == reg)
    return reg in (rd, ra)


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

    def __init__(self, code, base, mem, calls=None):
        self.code, self.base, self.mem = code, base, mem
        self.r = [0] * 32
        self.cr0 = 0
        self.crf = {}              # the other CR fields a compare set, by number: -1, 0 or 1
        self.written = set()
        self.calls = calls or {}   # round 6: a `bl` out of the code, by target: a mock answering r3
        self.called = []           # (target, r3 going in), in order

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
            elif op == 12:                                  # addic (rA is never 0; CA not kept)
                self.set(rd, self.r[ra] + simm)
            elif op == 7:                                   # mulli
                self.set(rd, signed(self.r[ra]) * simm)
            elif op == 24:                                  # ori (rS, rA swapped)
                self.set(ra, self.r[rd] | imm)
            elif op == 27:                                  # xoris (rS, rA swapped)
                self.set(ra, self.r[rd] ^ (imm << 16))
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
            elif op == 31 and ((w >> 1) & 0x3FF) == 23:     # lwzx
                self.set(rd, self.ld((base + self.r[rb]) & 0xFFFFFFFF, 4))
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
            elif op == 11 and (rd & 3) == 0:                # cmpwi crN
                a = signed(self.r[ra])
                self.crf[rd >> 2] = -1 if a < simm else (1 if a > simm else 0)
                self.written.add("cr%d" % (rd >> 2))
            elif op == 31 and ((w >> 1) & 0x3FF) == 215:    # stbx
                self.st(((self.r[ra] if ra else 0) + self.r[rb]) & 0xFFFFFFFF, self.r[rd], 1)
            elif op == 31 and ((w >> 1) & 0x3FF) == 87:     # lbzx
                self.set(rd, self.ld(((self.r[ra] if ra else 0) + self.r[rb]) & 0xFFFFFFFF, 1))
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
            elif op == 18 and (w & 3) == 1:                 # bl out of the code: a call the check mocks
                li = w & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                t = (pc + li) & 0xFFFFFFFF
                if t not in self.calls:
                    raise ValueError("0x%08X: a call to 0x%08X the check has no mock for" % (pc, t))
                self.called.append((t, self.r[3]))
                self.set(3, self.calls[t](self))
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
    ok(text == hdr["MH_ENTRY_LABEL"].encode() and all(cpu.r[i] == keep[i] for i in range(32) if i != 3),
       "help label stub: r3 = 0x%08X, \"%s\" in the data image, every other register kept" %
       (lab, text.decode("ascii", "replace")))


# ------------------------------------------- Deadlocked's GAME SETTINGS (round 3)

# MenuAudioVisual (screen 12, Options > GAME SETTINGS) on NPEA00423.
DL_AV_CREATE = (0x0038F664, 0x0038FA60)    # MenuAudioVisual_CreateWidgets
DL_AV_INIT = (0x0039063C, 0x00390A40)      # its GuiState's Init
DL_AV_DEINIT = (0x00390A40, 0x00390AF0)    # and Deinit
DL_AV_START, DL_AV_STOP = 0x0038F3D4, 0x0038F454
DL_AV_TABLE = 0x009EA038                    # MenuAudioVisual_Options[8] {bEnabled, tgt, name, item, fcCMD}
DL_PLAY_SOUND = 0x0035FAC0
DL_SET_TEXT, DL_CLEAR_LAYER = 0x000B0CDC, 0x000B299C
DL_BONUS = os.path.join("Documents", "rpcs3", "dev_hdd0", "game", "NPEA00423", "USRDIR", "rc4",
                        "ps3data", "global", "boot", "bonus.ps3")


def words_in(elf, lo, hi):
    return [(a, elf.word(a)) for a in range(lo, hi, 4)]


def sound_played(elf, lo, hi, snd):
    """`li r3,snd` and, within six words that load nothing else into r3, `bl GuiMain_PlaySound`."""
    ws = words_in(elf, lo, hi)
    for i, (a, w) in enumerate(ws):
        if w == 0x38600000 | snd:
            for b, x in ws[i + 1:i + 7]:
                if (x >> 26) == 18 and branch_target(b, x) == DL_PLAY_SOUND:
                    return True
                if (x >> 26) in (14, 15, 32, 34) and ((x >> 21) & 31) == 3:
                    break
    return False


def dige_templates(path, which):
    """
    The widgets of templates `which` in Deadlocked's IGE layouts (bonus.ps3,
    "HBN_BEG.04.05.02"): a block is {u16 type, u16 count}; type 1 a text, 2 a
    rectangle, 4 a container (children until END_WIDGET_CONT), 5 the root, 3
    a shape with a vertex list, 9 a widget's end. Answers {template:
    {element: (kind, fields, parent)}}, or None when the file is not one.
    """
    d = open(path, "rb").read()
    if not d.startswith(b"HBN_BEG.04.05.02"):
        return None
    u = lambda a: struct.unpack(">I", d[a:a + 4])[0]
    f = lambda a: struct.unpack(">f", d[a:a + 4])[0]
    n = u(0x14)
    offs = [u(0x18 + 4 * i) for i in range(n)]
    out = {}
    for t in which:
        p, e = offs[t], offs[t + 1] if t + 1 < n else len(d)
        els, scope = {}, [None]
        while p < e:
            if d[p:p + 15] in (b"END_WIDGET_CONT", b"END_WIDGET_PCON"):
                if d[p:p + 15] == b"END_WIDGET_CONT" and len(scope) > 1:
                    scope.pop()
                p += 16
                continue
            ty = u(p) >> 16
            if ty == 9:
                p += 4
                continue
            idx = u(p + 8)
            if ty == 1:
                els[idx] = ("text", {"x": f(p + 12), "y": f(p + 16), "font": u(p + 20), "size": f(p + 24),
                                     "flags": u(p + 4), "shown": u(p + 36)}, scope[-1])
                p += 48
            elif ty == 2:
                els[idx] = ("rect", {"x": f(p + 12), "y": f(p + 16), "w": f(p + 20), "h": f(p + 24),
                                     "shown": u(p + 32), "rgba": u(p + 36)}, scope[-1])
                p += 40
            elif ty in (4, 5):
                els[idx] = ("cont" if ty == 4 else "root", {"shown": u(p + 32)}, scope[-1])
                p += 36
                if ty == 4:
                    scope.append(idx)
            elif ty in (3, 10):
                els[idx] = ("shape" if ty == 3 else "textarea", {}, scope[-1])
                p += 12
                while p < e and u(p) != 0x00090000:
                    p += 4
            else:
                return None
        out[t] = els
    return out


def check_rac4_settings(ok, elf, hdr):
    lo, hi = DL_AV_CREATE
    ws = [w for _, w in words_in(elf, lo, hi)]
    ok(0x38800000 | hdr["DL_TPL_SCREEN"] in ws,
       "MenuAudioVisual_CreateWidgets (0x%06X) makes GAME SETTINGS from template %d" % (lo, hdr["DL_TPL_SCREEN"]))
    for el, what in ((hdr["DL_EL_LEGEND"], "legend"), (hdr["DL_EL_TITLE"], "title")):
        at = [a for a, w in words_in(elf, lo, hi) if (w >> 26) == 12 and (w & 0xFFFF) == el]
        good = any(any((x >> 26) == 18 and branch_target(b, x) == DL_SET_TEXT for b, x in words_in(elf, a, a + 0x28))
                   for a in at)
        ok(good, "and sets its %s, element 0x%X, with SetText" % (what, el))
    ok(0x38E00000 | hdr["DL_TPL_ROW"] in [elf.word(0x00395C38 + 4 * i) for i in range(0, 0x700 // 4)],
       "the rows are template %d, as MenuCheats_CreateWidgets (0x395C38) makes its list's" % hdr["DL_TPL_ROW"])
    lo, hi = DL_AV_INIT
    plates = [v for a, v in refs_into(elf, 0x00ACA360, 0x00ACA3A0)[2] if lo <= a < hi]
    ok(set(plates) == {hdr["DL_PLATE_TOP"], hdr["DL_PLATE_BOTTOM"], hdr["DL_PLATE_FOOT"]},
       "its thread's Init (0x%06X) loads the three plates the shim loads, and no other" % lo)
    ok(any((w >> 26) == 12 and (w & 0xFFFF) == hdr["DL_EL_CURSOR"] for _, w in words_in(elf, lo, hi)),
       "and puts the frame and the cursor on element 0x%X" % hdr["DL_EL_CURSOR"])
    lo, hi = DL_AV_DEINIT
    ok(any((w >> 26) == 18 and branch_target(a, w) == DL_CLEAR_LAYER for a, w in words_in(elf, lo, hi)) and
       0x3860000F in [w for _, w in words_in(elf, lo, hi)] and hdr["DL_LAYER"] == 0xF,
       "its Deinit (0x%06X) clears canvas layer 15, the shim's" % lo)
    # The fixed rows' widgets, which the shim hides: the table's names and items, and the bars by them.
    rows = [struct.unpack(">5I", b"".join(struct.pack(">I", elf.word(DL_AV_TABLE + 20 * i + 4 * k))
                                          for k in range(5))) for i in range(8)]
    items = [r[3] for r in rows]
    names = [r[2] for r in rows]
    tgts = [r[1] for r in rows]
    lo_el, hi_el = hdr["DL_EL_ROWS_FIRST"], hdr["DL_EL_ROWS_LAST"]
    bars = [x for i in (2, 3, 4) for x in (items[i] + 1, items[i] + 2)]
    ok(tgts == list(range(0x11, 0x09, -1)) and all(lo_el <= x <= hi_el for x in items + bars) and
       sorted(items + bars + names) == list(range(0x15, 0x29)) + [0x2A, 0x2B],
       "GAME SETTINGS' row table (0x%08X): focus rectangles 0x11..0xA, the values, the bars and the labels "
       "0x15..0x2B but 0x29, of which the shim hides 0x%X..0x%X (0x2A and 0x2B are in 0x29)" %
       (DL_AV_TABLE, lo_el, hi_el))
    # The sounds the shim plays where GAME SETTINGS, the lists and the main menu play them.
    ok(sound_played(elf, DL_AV_START, DL_AV_START + 0x80, hdr["SND_SCREEN"]) and
       sound_played(elf, DL_AV_STOP, DL_AV_STOP + 0x48, hdr["SND_SCREEN_CLOSE"]),
       "GAME SETTINGS' start plays 0x%X and its stop 0x%X" % (hdr["SND_SCREEN"], hdr["SND_SCREEN_CLOSE"]))
    ok(sound_played(elf, 0x00390404, 0x003904A8, hdr["SND_CHANGE"]) and
       sound_played(elf, 0x0038FF28, 0x003900A8, hdr["SND_SLIDE"]) and
       sound_played(elf, 0x0038FF28, 0x003900A8, hdr["SND_LIMIT"]),
       "its HELP TEXT toggle plays 0x%X, its MUSIC VOLUME slider 0x%X, or 0x%X at an end" %
       (hdr["SND_CHANGE"], hdr["SND_SLIDE"], hdr["SND_LIMIT"]))
    ok(sound_played(elf, 0x003A6700, 0x003A6858, hdr["SND_OPEN"]) and
       sound_played(elf, hdr["DL_HELP_CB"], hdr["DL_HELP_CB"] + 0x80, hdr["SND_FORWARD"]) and
       sound_played(elf, 0x003A809C, 0x003A8100, hdr["SND_BACK"]),
       "MenuMain_StartMain plays %d, the Help row's callback %d, Options' back %d" %
       (hdr["SND_OPEN"], hdr["SND_FORWARD"], hdr["SND_BACK"]))
    flag = hdr["DL_MENU_SOUND_FLAG"]
    ok(elf.word(0x00361428) == 0x3C600000 | ((flag + 0x8000) >> 16) and
       elf.word(0x0036142C) == 0x88630000 | (flag & 0xFFFF) and elf.word(0x00361430) == 0x4E800020,
       "GuiMain's main menu sound flag (0x361428: lis, lbz, blr) is the byte at 0x%08X" % flag)
    ex = hdr["DL_MENU_EXIT"]
    starts = [a for a, v in refs_into(elf, ex, ex + 1)[2] if hdr["DL_START_MAIN"] <= a < 0x003A6858]
    helps = [a for a, v in refs_into(elf, ex, ex + 1)[2] if hdr["DL_HELP_CB"] <= a < hdr["DL_HELP_CB"] + 0x80]
    ok(starts and helps, "g_bMenuExit, 0x%08X, is the byte MenuMain_StartMain reads and the Help row's callback "
       "clears" % ex)


def header_float(name):
    m = re.search(r"#define\s+%s\s+([0-9.]+)f" % name, open(os.path.join(HERE, "mh_rac4.h")).read())
    return float(m.group(1)) if m else None


def check_rac4_templates(ok, elf, hdr, path):
    """Templates 37, 28 and 27 out of the PS3 build's own bonus.ps3, when it is at hand."""
    if not path or not os.path.isfile(path):
        print("        (no bonus.ps3: templates 37 and 28 left unread; --bonus FILE reads them)")
        return
    t = dige_templates(path, (hdr["DL_TPL_SCREEN"], hdr["DL_TPL_ROW"], 27))
    if t is None:
        ok(False, "%s is the IGE layouts, HBN_BEG.04.05.02" % path)
        return
    scr, row = t[hdr["DL_TPL_SCREEN"]], t[hdr["DL_TPL_ROW"]]
    kind = lambda els, i: els.get(i, ("none", {}, None))[0]
    ok(kind(scr, hdr["DL_EL_LIST"]) == "rect" and kind(scr, hdr["DL_EL_CURSOR"]) == "rect" and
       kind(scr, hdr["DL_EL_TITLE"]) == "text" and kind(scr, hdr["DL_EL_LEGEND"]) == "text" and
       all(kind(scr, hdr[k]) == "rect" for k in ("DL_EL_PLATE_TOP", "DL_EL_PLATE_BOTTOM", "DL_EL_PLATE_FOOT")),
       "bonus.ps3, template %d: the list's target, the frame and the three plates are rectangles, the title "
       "and the legend texts" % hdr["DL_TPL_SCREEN"])
    box = scr.get(hdr["DL_EL_LIST"], ("", {"y": 0, "h": 0}, None))[1]
    tops = sorted(scr[i][1]["y"] for i in range(0xA, 0x12) if kind(scr, i) == "rect")
    ok(len(tops) == 8 and box["y"] - box["h"] / 2 <= tops[0] - 0.02 and tops[-1] + 0.02 <= box["y"] + box["h"] / 2,
       "its element 5 (y %.4f, h %.4f) holds all eight of GAME SETTINGS' rows (%.4f..%.4f): eight rows of the "
       "list on screen" % (box["y"], box["h"], tops[0] if tops else 0, tops[-1] if tops else 0))
    hidden = [i for i in range(hdr["DL_EL_ROWS_FIRST"], hdr["DL_EL_ROWS_LAST"] + 1)]
    ok(all(kind(scr, i) in ("text", "rect") for i in hidden if i != 0x29) and kind(scr, 0x29) == "cont" and
       all(scr[i][2] == 0x29 for i in (0x2A, 0x2B)) and max(scr) == 0x2B,
       "elements 0x%X..0x%X are the fixed rows' texts and rectangles, 0x29 the container of the last two "
       "labels: hiding them hides every row of GAME SETTINGS" % (hdr["DL_EL_ROWS_FIRST"], hdr["DL_EL_ROWS_LAST"]))
    lab, val = row.get(hdr["DL_ROW_LABEL"]), row.get(hdr["DL_ROW_VALUE"])
    ok(lab and val and lab[0] == "text" and val[0] == "text" and
       all(scr[i][0] == "text" and scr[i][1]["font"] == lab[1]["font"] and
           abs(scr[i][1]["size"] - lab[1]["size"]) < 1e-4 for i in (0x15, 0x23)) and
       val[1]["font"] == lab[1]["font"],
       "template %d's label and value are texts in GAME SETTINGS' row font (%d) and size (%.4f)" %
       (hdr["DL_TPL_ROW"], lab[1]["font"] if lab else 0, lab[1]["size"] if lab else 0))
    ok(kind(row, hdr["DL_ROW_BOX_A"]) == "cont" and kind(row, hdr["DL_ROW_BOX_B"]) == "cont" and
       not row[hdr["DL_ROW_BOX_A"]][1]["shown"] and not row[hdr["DL_ROW_BOX_B"]][1]["shown"] and
       all(kind(row, hdr[k]) == "rect" for k in ("DL_ROW_OUTLINE", "DL_ROW_TRACK", "DL_ROW_SPARE", "DL_ROW_FILL")) and
       row[hdr["DL_ROW_OUTLINE"]][2] == row[hdr["DL_ROW_TRACK"]][2] == hdr["DL_ROW_BOX_A"] and
       row[hdr["DL_ROW_SPARE"]][2] == row[hdr["DL_ROW_FILL"]][2] == hdr["DL_ROW_BOX_B"],
       "its two hidden containers hold the outline and the track, and the spare and the fill, as the shim "
       "reshapes them")
    sx = header_float("DL_SLIDER_X") or 0
    o, tr, fl = scr[0x1E][1], scr[0x1F][1], scr[0x20][1]
    want_right = val[1]["x"] + (o["x"] + o["w"] / 2 - scr[0x22][1]["x"])
    ok(abs(sx + 0.3555 / 2 - want_right) < 0.001 and abs(o["w"] - 0.3555) < 1e-4 and abs(o["h"] - 0.0288) < 1e-4 and
       abs(tr["w"] - 0.3477) < 1e-4 and abs(tr["h"] - 0.0192) < 1e-4 and o["rgba"] == 0x80D0D0D0 and
       tr["rgba"] == hdr["DL_RGBA_TRACK"] and fl["rgba"] == 0x800606C3,
       "GAME SETTINGS' slider is an outline 0.3555 x 0.0288, a track 0.3477 x 0.0192 (0x%08X) and a fill, "
       "ending %.4f past its value column; the shim's ends as far past the row's (at %.4f)" %
       (tr["rgba"], o["x"] + o["w"] / 2 - scr[0x22][1]["x"], sx + 0.3555 / 2))
    # The list's box: the Cheats screen's width, GAME SETTINGS' rows' height, the scroll bar inside the frame.
    ch = t[27].get(5, ("", {"x": 0, "w": 0}, None))[1]
    l, tp, r, b = (header_float(n) for n in ("DL_LIST_L", "DL_LIST_T", "DL_LIST_R", "DL_LIST_B"))
    fr = scr[hdr["DL_EL_CURSOR"]][1]
    bar_lo = struct.unpack(">f", struct.pack(">I", elf.word(0x00633DF0)))[0]
    bar_w = struct.unpack(">f", struct.pack(">I", elf.word(0x00633DF4)))[0]
    ok(None not in (l, tp, r, b) and abs(l - (ch["x"] - ch["w"] / 2)) < 1e-4 and abs(r - (ch["x"] + ch["w"] / 2)) < 1e-4 and
       abs(tp - (box["y"] - box["h"] / 2)) < 1e-4 and abs(b - (box["y"] + box["h"] / 2)) < 1e-4 and
       r + bar_lo + bar_w < fr["x"] + fr["w"] / 2 < box["x"] + box["w"] / 2 + bar_lo,
       "the list's box %.4f..%.4f wide is the Cheats screen's list (template 27, element 5) and %.4f..%.4f high "
       "GAME SETTINGS' rows: CtlList's scroll bar (%.4f past the box, %.4f wide; 0x633DF0) ends inside the frame "
       "(%.4f), where on element 5 as it is it would not" % (l or 0, r or 0, tp or 0, b or 0, bar_lo, bar_w,
                                                               fr["x"] + fr["w"] / 2))


# ------------------------------------- Deadlocked's planet picker (mailbox version 3)

def check_rac4_picker(ok, elf, hdr, shim_defs, code, cave, stub_at):
    """
    Worlds (screen 3, MenuPlanet) as the picker's six words and the shim assume
    it: the code round each site, the numbers the shim reads, the main menu's
    WORLDS row's own rule, and the leaf gates stepped with the picker up and
    down on either outcome of the game's own test.
    """
    w = elf.word
    # X: HandleInput's command 1 reaches `bl HandleSelection`, then its epilogue.
    sel = hdr["MH_SITE_SELECT"]
    ok(branch_target(sel, hdr["MH_ORIG_SELECT"]) == hdr["DL_HANDLE_SELECTION"] and
       branch_target(sel + 4, w(sel + 4)) == hdr["DL_INPUT_DONE"] and
       w(0x003B27A0) == 0x2C030001 and branch_target(0x003B27A4, w(0x003B27A4)) == sel,
       "Worlds' X: command 1 (0x3B27A0) goes to 0x%08X, `bl HandleSelection` (0x%08X), then the epilogue" %
       (sel, hdr["DL_HANDLE_SELECTION"]))
    # Triangle (Cancel) and Start (SystemCancel): the active mission's test, then the press or the epilogue.
    back, close = hdr["MH_SITE_BACK"], hdr["MH_SITE_CLOSE"]
    ok(w(back - 12) == 0x3C80009D and w(back - 8) == 0x8084195C and w(back - 4) == 0x2C04FFFF and
       branch_target(back, hdr["MH_ORIG_BACK"]) == hdr["DL_INPUT_DONE"] and back + 4 == hdr["DL_BACK_TAKEN"] and
       w(hdr["DL_BACK_TAKEN"]) == 0x3C80011A and
       branch_target(hdr["DL_BACK_TAKEN"] + 12, w(hdr["DL_BACK_TAKEN"] + 12)) == 0x00362584,
       "Triangle's gate (0x%08X): mf_activeMissionIndex (0x9D195C) against -1, the press dropped (0x%08X) or on to "
       "PopScreen (0x362584)" % (back, hdr["DL_INPUT_DONE"]))
    ok(w(close - 12) == 0x3C60009D and w(close - 8) == 0x8063195C and w(close - 4) == 0x2C03FFFF and
       branch_target(close, hdr["MH_ORIG_CLOSE"]) == hdr["DL_INPUT_DONE"] and close + 4 == hdr["DL_CLOSE_TAKEN"] and
       branch_target(hdr["DL_CLOSE_TAKEN"], w(hdr["DL_CLOSE_TAKEN"])) == 0x00361408 and
       w(close + 0x30) == 0x3880FFFF and branch_target(close + 0x34, w(close + 0x34)) == 0x0036249C,
       "Start's gate (0x%08X): the same test, then the sound flag (0x361408), the three bytes, CloseMenu(p, -1): "
       "the close the shim's pick copies" % close)
    frm = hdr["DL_PLANET_FROM"]
    ok([w(close + 0x1C), w(close + 0x20), w(close + 0x24)] ==
       [0x98830000 | ((frm + 2) & 0xFFFF), 0x98850000 | ((frm + 1) & 0xFFFF), 0x98860000 | (frm & 0xFFFF)] and
       (frm + 0x8000) >> 16 == 0x11A,
       "the three bytes it clears are g_fromMissionScreen, g_fromOther, g_fromPlanet at 0x%08X" % frm)
    # The list: Ghost Station's test, and the count.
    gh, cnt = hdr["MH_SITE_GHOST"], hdr["MH_SITE_COUNT"]
    ok(w(gh - 0x1C) == 0x2C1E000E and branch_target(gh - 0x18, w(gh - 0x18)) == hdr["DL_GHOST_LISTED"] and
       w(gh - 4) == 0x2C030000 and branch_target(gh, hdr["MH_ORIG_GHOST"]) == hdr["DL_GHOST_TESTED"] and
       gh + 4 == hdr["DL_GHOST_LISTED"] and w(hdr["DL_GHOST_LISTED"]) == 0x2C1E0022 and
       (w(hdr["DL_GHOST_TESTED"]) >> 26) == 18 and (w(hdr["DL_GHOST_TESTED"]) & 1) == 1,
       "Ghost Station's gate (0x%08X): only entry 14 reaches it (0x%08X), a byte's bit deciding between "
       "listing it (0x%08X) and one more test (0x%08X)" % (gh, gh - 0x1C, hdr["DL_GHOST_LISTED"],
                                                             hdr["DL_GHOST_TESTED"]))
    ok(w(cnt) == hdr["MH_ORIG_COUNT"] and cnt + 4 == hdr["DL_COUNT_DONE"] and w(cnt + 4) == 0xEBC10030 and
       w(cnt + 16) == 0x4E800020 and w(0x003B1390) == 0x3880000F and w(0x003B13CC) == 0x908300B8 and
       branch_target(0x003B14B0, w(0x003B14B0)) == 0x003B1300 and
       not [a for a, _ in refs_into(elf, 0x003B1300, 0x003B1304)[0] if a != 0x003B14B0],
       "SetupPlanetTourneyList (0x3B1300, called from UpdatePlanets alone) answers 15 at 0x%08X, a leaf's "
       "epilogue after, having written a sixteenth entry, Interior (15, at +0xB8)" % cnt)
    ok(any(w(0x003B3200 + 4 * i) == 0x38A00013 for i in range(0x10)) and
       branch_target(0x003B323C, w(0x003B323C)) == 0x00633690,
       "and Worlds' CtlList holds 19 (CtlList::Create at 0x3B323C), room for the sixteenth")
    # The entry the shim reads: PlayerData's items, count and focus.
    pd, sz = hdr["DL_PLANET_PD"], hdr["DL_PLANET_PD_SIZE"]
    ok(w(0x003B27E8) == 0x1C630000 | sz and w(0x003B27EC) == 0x3C800000 | ((pd + 0x8000) >> 16) and
       w(0x003B27F4) == 0x30840000 | (pd & 0xFFFF) and w(0x003B281C) == 0x807F0000 | hdr["DL_PLANET_COUNT"] and
       w(0x003B2878) == 0x80A50000 | hdr["DL_PLANET_FOCUS"] and w(0x003B17F8) == 0x809F0000 | hdr["DL_PLANET_ITEMS"],
       "MenuPlanet_PlayerData at 0x%08X, 0x%X a player: its items +0x%X, count +0x%X, focus +0x%X" %
       (pd, sz, hdr["DL_PLANET_ITEMS"], hdr["DL_PLANET_COUNT"], hdr["DL_PLANET_FOCUS"]))
    # Worlds' table entry, and the WORLDS row's rule, which the shim's refusals copy.
    ok(hdr["MH_WORLDS_STOP"] == 0x009E8918 + 0x10 * hdr["DL_SCREEN_WORLDS"] + 0xC and
       w(hdr["MH_WORLDS_STOP"]) == hdr["MH_ORIG_WORLDS_STOP"] == hdr["OPD_WORLDS_STOP"],
       "0x%08X is GuiMenu_Screens[3].fcStop, MenuPlanet_Stop's descriptor" % hdr["MH_WORLDS_STOP"])
    lev, gate = hdr["DL_LEVEL"], hdr["DL_DZ_GATE"]
    ok(w(0x003A619C) == 0x3FC00000 | ((lev + 0x8000) >> 16) and w(0x003A61A0) == 0x83DE0000 | (lev & 0xFFFF) and
       w(0x003A61A4) == 0x2C1E0000 | hdr["DL_LEVEL_DREADZONE"] and w(0x003A61A8) == 0x2C9E0015 and
       w(0x003A61AC) == 0x2F1E0000 | hdr["DL_LEVEL_INTERIOR"] and w(0x003A61B0) == 0x2F9E0023 and
       w(0x003A61DC) == 0x3CA00000 | ((gate + 0x8000) >> 16) and w(0x003A61E8) == 0x30A50000 | (gate & 0xFFFF) and
       w(0x003A6394) == 0x30643FB0 and w(0x003A63B0) == 0x38800001,
       "the main menu greys WORLDS (descriptor 0x993FB0) in Interior (15, 35), and in DreadZone (1, 21) until "
       "the byte at 0x%08X (its mission 37's completions) is set: Level at 0x%08X, as the shim refuses" % (gate, lev))
    ok(w(0x00993FB0) == 0x003A8368 and w(0x00993FB4) == DL_TOC and
       any(w(0x003A8368 + 4 * i) == 0x38800000 | hdr["DL_SCREEN_WORLDS"] and
           any(branch_target(0x003A8368 + 4 * k, w(0x003A8368 + 4 * k)) == 0x00362308 for k in range(i + 1, i + 6))
           for i in range(0x18)),
       "and its callback (0x3A8368) is GuiMenu_PushScreen(p, 3), what the shim's PLANETS does")
    # The leaf gates, stepped.
    picker = hdr["MH_STATE"] + shim_defs["ST_PICKER"]
    gates = (("mh_gate_ghost", "DL_GHOST_TESTED", "DL_GHOST_LISTED", "Ghost Station listed"),
             ("mh_gate_back", "DL_INPUT_DONE", "DL_BACK_TAKEN", "Triangle pops"),
             ("mh_gate_close", "DL_INPUT_DONE", "DL_CLOSE_TAKEN", "Start closes"))
    for stub, taken, passed, what in gates:
        if stub not in stub_at:
            continue
        try:
            outs = []
            for eq in (1, 0):
                for up in (0, 1):
                    cpu = Ppc(code, cave, {})
                    cpu.st(picker, up, 1)
                    cpu.cr0 = 0 if eq else 1
                    cpu.r = [0x1000 + i for i in range(32)]
                    keep = list(cpu.r)
                    how = cpu.run(stub_at[stub])
                    clean = all(cpu.r[i] == keep[i] for i in range(32) if i != 12)
                    outs.append((how, clean))
            want = [("b", hdr[taken]), ("b", hdr[passed]), ("b", hdr[passed]), ("b", hdr[passed])]
            ok([o[0] for o in outs] == want and all(o[1] for o in outs),
               "%s stepped: the game's test true goes where the game goes with the picker down (0x%08X) and on "
               "with it up (0x%08X, %s); false, on as the game goes; only r12 and cr0 written" %
               (stub, hdr[taken], hdr[passed], what))
        except ValueError as e:
            ok(False, "%s steps: %s" % (stub, e))
    if "mh_gate_count" in stub_at:
        try:
            got = []
            for up in (0, 1):
                cpu = Ppc(code, cave, {})
                cpu.st(picker, up, 1)
                cpu.r = [0x1000 + i for i in range(32)]
                keep = list(cpu.r)
                how = cpu.run(stub_at["mh_gate_count"])
                got.append((how, cpu.r[3], all(cpu.r[i] == keep[i] for i in range(32) if i not in (3, 12))))
            ok(got == [(("b", hdr["DL_COUNT_DONE"]), 15, True), (("b", hdr["DL_COUNT_DONE"]), 16, True)],
               "mh_gate_count stepped: 15, or 16 with the picker up, on to the epilogue; only r3, r12 and cr0 "
               "written")
        except ValueError as e:
            ok(False, "mh_gate_count steps: %s" % e)
    # The pickers' round 2: a planet's lock in UpdatePlanets, and Circle's Skills in HandleInput.
    lk = hdr["MH_SITE_LOCKED"]
    ok(w(0x003B14DC) == 0x80740008 and w(0x003B14E4) == 0x2C030000 and
       branch_target(0x003B14F0, w(0x003B14F0)) == 0x003B1650 and
       w(0x003B1548) == 0x88840300 and w(0x003B154C) == 0x548907BC and
       w(lk) == hdr["MH_ORIG_LOCKED"] == 0x7CA7C1AE and lk + 4 == hdr["DL_LOCKED_DONE"] and
       w(0x003B1504) == 0x63670000 and w(0x003B14CC) == 0x3F60011A and w(0x003B14D0) == 0x337B1374,
       "the lock's site (0x%08X) is on UpdatePlanets' path for a planet (an entry's type 0, 0x3B14E4), after "
       "the level's status byte (+0x300, 0x3B1548) and its bit 0x02 (0x3B154C): stbx r5,r7,r24, bLocked into the "
       "list at 0x11A1374 (r7 = r27)" % lk)
    ok(w(lk + 4) == 0x39000000 and w(lk + 8) == 0x39200001 and w(lk + 12) == 0x7884D146 and
       w(lk + 16) == 0x38A00001 and
       w(0x003B15CC) == 0x41860008 and w(0x003B15D0) == 0x409A0190 and
       not any((w(a) >> 26) == 16 and ((w(a) >> 18) & 7) == 0 for a in range(lk + 4, 0x003B15D4, 4)) and
       w(0x003B1610) == 0x2C050000 and w(0x003B1760) == 0x2C1E000E and
       not [a for a in range(0x003B1440, 0x003B1BB0, 4) if uses_reg(w(a), 12)],
       "after it r5 is set again (li r5,1, 0x%08X) before it is read, no branch reads cr0 before 0x3B1610 or "
       "0x3B1760 sets it again, and the function never writes r12: the gate's r5, r12 and cr0 are free" % (lk + 16))
    ok(w(0x003B16C4) == 0x7C9BC1AE and w(0x003B1828) == 0x3B607548 and w(0x003B1804) == 0x88740000,
       "a tournament's lock is the other store (0x3B16C4), which stays the game's; the list's icon is the lock "
       "(0x7548, 0x3B1828) on bLocked (0x3B1804)")
    sk = hdr["MH_SITE_SKILLS"]
    ok(w(sk - 4) == 0x2C030004 and branch_target(sk, hdr["MH_ORIG_SKILLS"]) == hdr["DL_SKILLS_TAKEN"] and
       sk + 4 == hdr["DL_SKILLS_NEXT"] and w(hdr["DL_SKILLS_NEXT"]) == 0x2C030002 and
       branch_target(hdr["DL_SKILLS_TAKEN"], w(hdr["DL_SKILLS_TAKEN"])) == 0x000B2244 and
       w(hdr["DL_SKILLS_TAKEN"] + 0x20) == 0x38800015 and
       branch_target(hdr["DL_SKILLS_TAKEN"] + 0x34, w(hdr["DL_SKILLS_TAKEN"] + 0x34)) == 0x00362308 and
       branch_target(hdr["DL_SKILLS_TAKEN"] + 0x3C, w(hdr["DL_SKILLS_TAKEN"] + 0x3C)) == hdr["DL_INPUT_DONE"] and
       w(0x003B2768) == 0x38600006 and branch_target(0x003B2770, w(0x003B2770)) == 0x00360958,
       "Circle's gate (0x%08X): GuiMain_GetMenuCommand(6)'s command 4 goes to 0x%08X, GetCurrentCanvas then "
       "GuiMenu_PushScreen(p, 21), the Skills screen; any other command to the next test (0x%08X)" %
       (sk, hdr["DL_SKILLS_TAKEN"], hdr["DL_SKILLS_NEXT"]))
    if "mh_gate_locked" in stub_at:
        try:
            good = True
            for up in (0, 1):
                for r5 in (0, 1):
                    cpu = Ppc(code, cave, {})
                    cpu.st(picker, up, 1)
                    cpu.r = [0x1000 + i for i in range(32)]
                    cpu.r[5], cpu.r[7], cpu.r[24] = r5, 0x011A1374, 0x18
                    cpu.st(0x011A1374 + 0x18, 0x77, 1)
                    keep = list(cpu.r)
                    how = cpu.run(stub_at["mh_gate_locked"])
                    good = good and how == ("b", hdr["DL_LOCKED_DONE"]) and \
                        cpu.ld(0x011A1374 + 0x18, 1) == (0 if up else r5) and \
                        all(cpu.r[i] == keep[i] for i in range(32) if i not in (5, 12)) and \
                        cpu.r[5] == (0 if up else r5) and cpu.written <= {"r5", "r12", "cr0"}
            ok(good, "mh_gate_locked stepped: the entry's bLocked the game's (locked or not) with the picker down, "
               "0 with it up, on to 0x%08X; only r5, r12 and cr0 written" % hdr["DL_LOCKED_DONE"])
        except ValueError as e:
            ok(False, "mh_gate_locked steps: %s" % e)
    if "mh_gate_skills" in stub_at:
        try:
            outs = []
            for eq in (1, 0):
                for up in (0, 1):
                    cpu = Ppc(code, cave, {})
                    cpu.st(picker, up, 1)
                    cpu.cr0 = 0 if eq else 1
                    cpu.r = [0x1000 + i for i in range(32)]
                    keep = list(cpu.r)
                    how = cpu.run(stub_at["mh_gate_skills"])
                    outs.append((how, all(cpu.r[i] == keep[i] for i in range(32) if i != 12)))
            ok([o[0] for o in outs] == [("b", hdr["DL_SKILLS_TAKEN"]), ("b", hdr["DL_INPUT_DONE"]),
                                         ("b", hdr["DL_SKILLS_NEXT"]), ("b", hdr["DL_SKILLS_NEXT"])] and
               all(o[1] for o in outs),
               "mh_gate_skills stepped: Circle goes to Skills (0x%08X) with the picker down and is ignored "
               "(0x%08X) with it up; another command on to its test (0x%08X) either way; only r12 and cr0 "
               "written, r3 kept" % (hdr["DL_SKILLS_TAKEN"], hdr["DL_INPUT_DONE"], hdr["DL_SKILLS_NEXT"]))
        except ValueError as e:
            ok(False, "mh_gate_skills steps: %s" % e)
    check_rac4_round6(ok, elf, hdr, code, cave, stub_at, picker)
    # The select stub: the frame stub's shape, mh_worlds_select with the module's TOC, else HandleSelection.
    at = stub_at.get("mh_select_stub")
    if at is not None:
        ws = [struct.unpack(">I", code[at - cave + 4 * i:at - cave + 4 * i + 4])[0] for i in range(15)]
        ok(ws[0:4] == [0xF821FF81, 0x7C0802A6, 0xF8010090, 0xF8410070] and (ws[4] >> 16) == 0x3C40 and
           (ws[5] >> 16) == 0x3842 and (ws[6] >> 26) == 18 and (ws[6] & 3) == 1 and ws[7] == 0x60000000 and
           ws[8:14] == [0x2C030000, 0xE8410070, 0xE8010090, 0x7C0803A6, 0x38210080, 0x4C820020] and
           branch_target(at + 56, ws[14]) == hdr["DL_HANDLE_SELECTION"] and (ws[14] & 3) == 0,
           "mh_select_stub: a frame, r2 the module's TOC, bl mh_worlds_select, the game's r2 and LR back, bnelr, "
           "else b HandleSelection (0x%08X)" % hdr["DL_HANDLE_SELECTION"])


def check_rac4_round6(ok, elf, hdr, code, cave, stub_at, picker):
    """
    Round 6's two words in the Trainer's Worlds: the tournaments left out of its
    list (UpdatePlanets' type test, mh_gate_tourney) and SKILLS out of its legend
    (CreateWidgets' mission test, mh_gate_legend): the code round each site, and
    the stubs stepped with the picker up and down.
    """
    w = elf.word
    ts = hdr["MH_SITE_TOURNEY"]
    ok(w(ts - 0x18) == 0x83D40004 and w(ts - 0x14) == 0x80740008 and w(ts - 0x10) == 0x7FDE07B4 and
       w(ts - 0xC) == 0x2C030000 and w(ts - 8) == 0x2C9E0001 and w(ts - 4) == 0x2F1E0015 and
       w(ts) == hdr["MH_ORIG_TOURNEY"] and branch_target(ts, w(ts)) == hdr["DL_TOURNEY_TAKEN"] and
       ts + 4 == hdr["DL_TOURNEY_PLANET"] and w(0x003B14CC) == 0x3F60011A and w(0x003B14D0) == 0x337B1374,
       "the tournaments' site (0x%08X) in UpdatePlanets' loop over the entries at 0x11A1374: r30 the entry's iLink "
       "(+4, extsw), cr0 its type (+8) against 0, cr1 and cr6 set before it for the planet's path (0x%08X); "
       "type 1 to 0x%08X" % (ts, hdr["DL_TOURNEY_PLANET"], hdr["DL_TOURNEY_TAKEN"]))
    tk, nx = hdr["DL_TOURNEY_TAKEN"], hdr["DL_ENTRY_NEXT"]
    ok(w(tk) == 0x63C30000 and (w(tk + 4) >> 26) == 18 and (w(tk + 4) & 3) == 1 and
       branch_target(tk + 4, w(tk + 4)) == 0x003B11C0 and
       w(nx) == 0x339C0001 and w(nx + 4) == 0x3318000C and w(nx + 8) == 0x7C1CE800 and
       branch_target(nx + 12, w(nx + 12)) == 0x003B14CC and
       branch_target(0x003B17C0, w(0x003B17C0)) == 0x00633B40 and
       not [a for a in range(0x003B1440, 0x003B1BB0, 4)
            if a != 0x003B17C0 and branch_target(a, w(a)) == 0x00633B40],
       "a tournament's path (0x%08X) calls MenuPlanet_GetTournamentHubMission (0x3B11C0) before it reads cr0; "
       "the loop's next entry (0x%08X) counts on, steps 12 bytes, compares (cr0) and goes back to 0x3B14CC, "
       "which reloads the table; CtlList::Insert (0x633B40) is called once, at 0x3B17C0, between the two: an "
       "entry the gate sends on is not listed" % (tk, nx))
    ok(w(0x003B1308) == 0x38A00000 and w(0x003B1310) == 0x39000001 and w(0x003B1320) == 0x90A30004 and
       w(0x003B1324) == 0x91030008 and w(0x003B1334) == 0x90A30014 and
       w(0x003B2E64) == 0x2C830000 and branch_target(0x003B2E84, w(0x003B2E84)) == 0x003B2EE4 and
       (w(0x003B2E84) >> 16) == 0x4086 and w(0x003B2E88) == 0x38600001,
       "SetupPlanetTourneyList's first entry is tournament 0 (iLink 0, type 1), which "
       "MenuPlanet_GetTournamentName (0x3B2E50) names after level 1, DreadZone Station: the row the gate keeps")
    if "mh_gate_tourney" in stub_at:
        try:
            good = True
            for planet in (1, 0):
                for up in (0, 1):
                    for link in (0, 1, 2, 3, 4):
                        cpu = Ppc(code, cave, {})
                        cpu.st(picker, up, 1)
                        cpu.cr0 = 0 if planet else 1
                        cpu.r = [0x1000 + i for i in range(32)]
                        cpu.r[30] = link
                        keep = list(cpu.r)
                        how = cpu.run(stub_at["mh_gate_tourney"])
                        if planet:
                            want = ("b", hdr["DL_TOURNEY_PLANET"])
                            clean = cpu.written == set()
                        else:
                            want = ("b", hdr["DL_ENTRY_NEXT"] if up and link else hdr["DL_TOURNEY_TAKEN"])
                            clean = cpu.written <= {"r12", "cr0"} and \
                                all(cpu.r[i] == keep[i] for i in range(32) if i != 12)
                        good = good and how == want and clean
            ok(good, "mh_gate_tourney stepped: a planet on its way (0x%08X) with nothing written; a tournament to its "
               "path (0x%08X) with the picker down, and with it up DreadZone Station (iLink 0) too, tournaments 1 "
               "to 4 on to the next entry (0x%08X); only r12 and cr0 written" %
               (hdr["DL_TOURNEY_PLANET"], hdr["DL_TOURNEY_TAKEN"], hdr["DL_ENTRY_NEXT"]))
        except ValueError as e:
            ok(False, "mh_gate_tourney steps: %s" % e)

    lg, two, three, prn = hdr["MH_SITE_LEGEND"], hdr["DL_LEGEND_TWO"], hdr["DL_LEGEND_THREE"], hdr["DL_LEGEND_PRINT"]
    fn, fmt = hdr["DL_MSG_STRING_FN"], hdr["DL_LEGEND_FMT"]
    ok(w(lg - 12) == 0x3C60009D and w(lg - 8) == 0x8063195C and w(lg - 4) == 0x2C03FFFF and
       w(lg) == hdr["MH_ORIG_LEGEND"] and branch_target(lg, w(lg)) == three and lg + 4 == two and
       w(two) == 0x3C60009C and w(two + 4) == 0x3C800000 | (fmt >> 16) and w(two + 8) == 0x33A40000 | (fmt & 0xFFFF) and
       w(two + 0xC) == 0xA063E872 and branch_target(two + 0x14, w(two + 0x14)) == fn and
       w(two + 0x24) == 0xA064E9D8 and branch_target(two + 0x2C, w(two + 0x2C)) == fn and two + 0x34 == prn and
       w(three) == 0x3C60009C and w(three + 8) == 0x33A40000 | ((fmt + 8) & 0xFFFF) and
       w(three + 0x3C) == 0xA064E9C4,
       "the legend's site (0x%08X) in MenuPlanet_CreateWidgets: mf_activeMissionIndex (0x9D195C) against -1; "
       "none active, \"%%s %%s\" (0x%08X) with the texts at 0x9BE872 and 0x9BE9D8 (0x%08X); one, \"%%s %%s %%s\" "
       "with 0x9BE9C4's too (0x%08X); msg_string (0x%08X) fetches each" % (lg, fmt, two, three, fn))
    ok((w(0x009BE870) & 0xFFFF) == 0x2EE9 and (w(0x009BE9D8) >> 16) == hdr["DL_MSG_SELECT"] and
       (w(0x009BE9C4) >> 16) == hdr["DL_MSG_BACK"] and w(fmt) == 0x25732025 and (w(fmt + 4) >> 16) == 0x7300 and
       w(fmt + 8) == 0x25732025 and w(fmt + 12) == 0x73202573,
       "the texts are 0x2EE9 (\"\\x11 SKILLS\"), 0x%04X (SELECT) and 0x%04X (BACK), GAME SETTINGS' two; the "
       "formats \"%%s %%s\" and \"%%s %%s %%s\"" % (hdr["DL_MSG_SELECT"], hdr["DL_MSG_BACK"]))
    ok(w(prn) == 0x60660000 and w(prn + 4) == 0x63E30000 and w(prn + 8) == 0x63A40000 and w(prn + 12) == 0x63850000 and
       branch_target(prn + 16, w(prn + 16)) == 0x007EDA3C and branch_target(prn + 24, w(prn + 24)) == 0x003AE5BC and
       w(0x003AE4DC) == 0x33E31730 and w(0x003AE4E0) == 0x30640011 and w(0x003AE4E4) == 0x63E40000 and
       branch_target(0x003AE4EC, w(0x003AE4EC)) == 0x000B0CDC and
       not [a for a in range(0x003AE4F0, prn, 4) if 31 in written_regs(w(a))],
       "the print (0x%08X): sprintf(r31, r29, r28, r3), r31 the legend's buffer 0x11A1730, which SetText gave the "
       "panel's element 0x11 before (0x3AE4EC), so the text it shows is whatever goes in; then on to 0x3AE5BC" % prn)
    first = {}
    for a in range(0x003AE5BC, 0x003AE97C, 4):
        for r in (28, 29):
            if r not in first and uses_reg(w(a), r):
                first[r] = w(a)
    callers = [a for a, _ in refs_into(elf, 0x003AE468, 0x003AE46C)[0]]
    ok(all(r in first and written_regs(first[r]) == (r,) and ((first[r] >> 21) & 31) != r for r in (28, 29)) and
       not [a for a in range(0x003AE468, 0x003AE97C, 4) if uses_reg(w(a), 12)] and
       callers == [0x003B3218] and w(0x009943E8 + 0x20) == 0x003B31D0,
       "after it r28 and r29 are written before they are read, the function never names r12, and only Worlds' "
       "init (0x3B31D0, from 0x3B3218) calls it: the gate's r3, r12, r28 and r29 are free, and it acts on Worlds "
       "alone")
    if "mh_gate_legend" in stub_at:
        try:
            good = True
            mock = {fn: lambda c: 0x00C00000 + c.r[3]}
            for up in (0, 1):
                for mission in (0xFFFFFFFF, 2):
                    cpu = Ppc(code, cave, {}, mock)
                    cpu.st(picker, up, 1)
                    cpu.r = [0x1000 + i for i in range(32)]
                    cpu.r[3] = mission
                    keep = list(cpu.r)
                    how = cpu.run(stub_at["mh_gate_legend"])
                    if not up:
                        good = good and how == ("b", two if mission == 0xFFFFFFFF else three) and \
                            cpu.called == [] and cpu.written <= {"r12", "cr0"} and \
                            all(cpu.r[i] == keep[i] for i in range(32) if i != 12)
                    else:
                        good = good and how == ("b", prn) and \
                            cpu.called == [(fn, hdr["DL_MSG_SELECT"]), (fn, hdr["DL_MSG_BACK"])] and \
                            cpu.r[28] == 0x00C00000 + hdr["DL_MSG_SELECT"] and \
                            cpu.r[3] == 0x00C00000 + hdr["DL_MSG_BACK"] and cpu.r[29] == fmt and \
                            cpu.written <= {"r3", "r12", "r28", "r29", "cr0"} and \
                            all(cpu.r[i] == keep[i] for i in range(32) if i not in (3, 12, 28, 29))
            ok(good, "mh_gate_legend stepped: with the picker down the game's legend, SKILLS and SELECT (0x%08X) or "
               "and BACK (0x%08X), only r12 and cr0 written; with it up msg_string(SELECT) into r28 and "
               "msg_string(BACK) into r3, \"%%s %%s\" into r29, on to the print (0x%08X)" % (two, three, prn))
        except ValueError as e:
            ok(False, "mh_gate_legend steps: %s" % e)


# --------------------------------------------------------------- the checks

def check_rac4(ok, elf, roots, bonus=None):
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
    ok(box[8] == hdr["MH_ROWS"] and box[9] == hdr["MH_ROW_BYTES"] == box_defs["MBOX_ROW_SIZE"] and
       box[10] == hdr["MH_MAIN_ROWS"] and box[11] == hdr["MH_CAPS"] and box[12] == 0 and box[13] == 0 and
       box[14] == hdr["MH_GAME_ID"] and box[15] == hdr["MH_CFG_ROWS"],
       "row_cap %d, rows of 0x%02X (round 4: no descriptions), %d rows a MAIN page, %d a CONFIG page, caps 0x%02X, "
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
    # Mailbox version 3, the planet picker: Worlds' X (a bl) and its eight gates (each a b; the lock and Skills
    # since the pickers' round 2, the tournaments and the legend since round 6).
    sites[hdr["MH_SITE_SELECT"]] = ("mh_select_stub", hdr["MH_ORIG_SELECT"])
    for g in ("GHOST", "COUNT", "BACK", "CLOSE", "LOCKED", "SKILLS", "TOURNEY", "LEGEND"):
        sites[hdr["MH_SITE_" + g]] = ("mh_gate_" + g.lower(), hdr["MH_ORIG_" + g])
    ok(len(hooks) == 19 and [h[0] for h in hooks] ==
       [slot, slot + 4, slot + 8, slot + 12, hdr["MH_SITE_FRAME"], hdr["MH_SITE_SCREEN"],
        hdr["MH_SITE_INJECT"], hdr["MH_SITE_SELECT"], hdr["MH_SITE_GHOST"], hdr["MH_SITE_COUNT"],
        hdr["MH_SITE_BACK"], hdr["MH_SITE_CLOSE"], hdr["MH_SITE_LOCKED"], hdr["MH_SITE_SKILLS"],
        hdr["MH_SITE_TOURNEY"], hdr["MH_SITE_LEGEND"],
        hdr["MH_WORLDS_STOP"], hdr["MH_SITE_HELP_LABEL"], hdr["MH_SITE_HELP_SCREEN"]],
       "nineteen words: screen 36's slot, then the frame, screen and inject hooks, then the planet picker's ten "
       "(Worlds' X, its eight gates, its stop), then the Help row's two")
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
    ok(hooks[-1][1] == hdr["MH_HELP_SCREEN_WORD"] == 0x38800000 | hdr["MH_SCREEN_ID"],
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

    # 4b. the CONFIG page's game knowledge, against GAME SETTINGS' own code (round 3)
    check_rac4_settings(ok, elf, hdr)
    vt = [v for a, v in refs_into(elf, hdr["DL_CTLLIST_VTABLE"], hdr["DL_CTLLIST_VTABLE"] + 4)[2]
          if 0x00633618 <= a < 0x00633650]
    ok(vt == [hdr["DL_CTLLIST_VTABLE"]] and elf.word(0x0063363C) == 0x907F0000,
       "CtlList::CtlList (0x633618) writes the vtable 0x%08X at +0, as the shim does" % hdr["DL_CTLLIST_VTABLE"])
    check_rac4_templates(ok, elf, hdr, bonus)

    stub_at = {}
    for a, value, orig, what in hooks:
        if a not in sites:
            continue
        stub, _ = sites[a]
        link = not stub.startswith("mh_gate_")
        t = branch_target(a, value) if (value >> 26) == 18 and (value & 3) == (1 if link else 0) else None
        first = struct.unpack(">I", code[t - cave:t - cave + 4])[0] if t is not None and cave <= t < cave + len(code) else None
        mask, want, name = STUB_FIRST[stub]
        good = t is not None and first is not None and (first & mask) == want and stub in what
        ok(good, "0x%08X: %08X = %s 0x%08X, %s (%s)" % (a, value, "bl" if link else "b", t or 0, stub, name))
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

    # Worlds' stop: a descriptor of the renderer's in place of MenuPlanet_Stop's (mailbox version 3).
    desc = words[hdr["MH_WORLDS_STOP"]]
    if dat <= desc <= dat + len(data) - 8:
        c, t = struct.unpack(">II", data[desc - dat:desc - dat + 8])
        ok(cave <= c < cave + len(code) and dat + 0x8000 <= t <= dat + len(data) + 0x8000,
           "Worlds' fcStop 0x%08X becomes a descriptor {0x%08X in the code, TOC 0x%08X}" % (desc, c, t))
    else:
        ok(False, "Worlds' fcStop 0x%08X is in the data image" % desc)

    # 4. the code, 5. the descriptors
    picker_outs = dict((hdr[k], k) for k in ("DL_HANDLE_SELECTION", "DL_GHOST_LISTED", "DL_GHOST_TESTED",
                                              "DL_COUNT_DONE", "DL_BACK_TAKEN", "DL_CLOSE_TAKEN", "DL_INPUT_DONE",
                                              "DL_LOCKED_DONE", "DL_SKILLS_TAKEN", "DL_SKILLS_NEXT",
                                              "DL_TOURNEY_PLANET", "DL_TOURNEY_TAKEN", "DL_ENTRY_NEXT",
                                              "DL_LEGEND_TWO", "DL_LEGEND_THREE", "DL_LEGEND_PRINT"))
    calls_out = (hdr["DL_MSG_STRING_FN"],)
    allowed = (hdr["GAME_HANDLE_MENU_INPUT"],) + tuple(picker_outs) + calls_out
    bad = lint_code(code, cave, allowed=allowed)
    ok(not bad, "the code: no branch out but the stubs' tail branches and the legend stub's calls, no sc, no "
       "vector instruction, no write to r13")
    for b in bad[:12]:
        print("        " + b)
    outs = [cave + i for i in range(0, len(code), 4)
            if branch_target(cave + i, struct.unpack(">I", code[i:i + 4])[0]) in allowed]
    tails = [o for o in outs
             if branch_target(o, struct.unpack(">I", code[o - cave:o - cave + 4])[0]) not in calls_out]
    calls = [o for o in outs if o not in tails]
    leg = stub_at.get("mh_gate_legend", -1)
    ok(len(tails) == 19 and all((struct.unpack(">I", code[o - cave:o - cave + 4])[0] & 3) == 0 and o < core[0]
                                for o in tails) and
       len(calls) == 2 and all((struct.unpack(">I", code[o - cave:o - cave + 4])[0] & 3) == 1 and
                               leg <= o < leg + 0x40 for o in calls),
       "nineteen ways out, every one a plain branch (no link) from a stub, before the core: the frame stub's tail "
       "call to HandleMenuInput, the select stub's to HandleSelection, and the gates' seventeen back into Worlds' "
       "code (the lock's one and Skills' three since the pickers' round 2, the tournaments' three and the "
       "legend's three since round 6); and the legend stub's two calls of msg_string (0x%08X), the only calls "
       "into the game that are not through a descriptor" % hdr["DL_MSG_STRING_FN"])

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

    # 4c, 8. the planet picker: Worlds' code round each word, the game's facts the shim reads, the stubs stepped
    check_rac4_picker(ok, elf, hdr, shim_defs, code, cave, stub_at)

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
    "mh_map_list_stub": (0xFFFF0000, 0x3D800000, "lis r12,..."),
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
    front = state + shim_defs["ST_FRONT"]
    text0, set_size = shim_set_layout(shim_defs, hdr["MH_ROWS"])
    text0 += state
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

    # The label hook: TRAINER, the rows, and everything else on into msg_string.
    msg_ok = {"r3", "r4", "r5", "cr0"}
    cpu, how = run("mh_msg_stub", {3: hdr["MH_MSG_HELP"], 4: 0x1234})
    ok(how == ("blr", None) and cstr(cpu.r[3]) == hdr["MH_ENTRY_LABEL"] and cpu.written <= msg_ok,
       "msg stub, 0xF000: \"%s\", in the data image, back to msg_string's caller" % hdr["MH_ENTRY_LABEL"])
    for f in (0, 1):
        good = True
        for i in range(hdr["MH_MAIN_ROWS"]):
            cpu, how = run("mh_msg_stub", {3: hdr["MH_MSG_ROW0"] + i}, {(front, 1): f})
            good = good and how == ("blr", None) and cpu.r[3] == text0 + f * set_size + i * text_len and                 cpu.written <= msg_ok
        ok(good, "msg stub, 0xF001 + i for each of the %d MAIN rows: row i of text set %d, the one on show "
           "(mh_text(i))" % (hdr["MH_MAIN_ROWS"], f))
    # The pickers' round 2: the Trainer's map's names, PLANET_LIST's, from the table at MH_MAP_NAMES.
    names = rac3_planet_names()
    image = dict(((hdr["MH_DATA"] + i, 1), b) for i, b in enumerate(data))
    good = len(names) >= hdr["MH_MAP_PLANETS"] == 30
    for p in range(hdr["MH_MAP_PLANETS"]):
        cpu, how = run("mh_msg_stub", {3: hdr["MH_MSG_PLANET"] + p, 4: 0x1234}, image)
        good = good and how == ("blr", None) and p < len(names) and cstr(cpu.r[3]) == names[p] and \
            cpu.written <= msg_ok
    ok(good, "msg stub, 0x%X + p for each of the map's %d planets: PLANET_LIST's name of planet p "
       "(src/games/rac3.c), from the table at 0x%08X in the data image" %
       (hdr["MH_MSG_PLANET"], hdr["MH_MAP_PLANETS"], hdr["MH_MAP_NAMES"]))
    for idn in (hdr["MH_MSG_ROW0"] + hdr["MH_MAIN_ROWS"], 0xF00F, 0xF010, 0xEFFF, 0x106, 0x522,
                hdr["MH_MSG_PLANET"] - 1, hdr["MH_MSG_PLANET"] + hdr["MH_MAP_PLANETS"],
                hdr["MH_MSG_PLANET"] + 0x3F, hdr["MH_MSG_PLANET"] + 0x40, 0xFFFFF040, 0x1F040):
        cpu, how = run("mh_msg_stub", {3: idn, 4: 0x1234}, image)
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

    # The GALACTIC MAP's list build, its filter's first word: the save's planet, or the Trainer's.
    flag = shim + hdr["SH_MAP"]
    image = dict(((hdr["MH_DATA"] + i, 1), b) for i, b in enumerate(data))
    map_ok = {"r3", "r12", "cr0", "cr1"}
    want = map_list(hdr)
    good = True
    for p in (0, 1, 2, 6, 20, 29, 30, 37, 0xFFFFFFFF):
        for i in (0, 5, 59):
            cpu, how = run("mh_map_list_stub", {3: p, 7: i, 29: 3, 6: 0x00DA5A94 + 4 * i})
            good = good and how == ("blr", None) and cpu.r[3] == p and cpu.r[6] == 0x00DA5A94 + 4 * i and \
                cpu.crf.get(1) == (-1 if (p - (1 << 32) if p & 0x80000000 else p) < 30 else
                                   (1 if (p - (1 << 32) if p & 0x80000000 else p) > 30 else 0)) and \
                cpu.written <= map_ok
    ok(good, "map list stub, SH_MAP down (the game's own map): the game's word, cmpwi cr1,r3,30, r3 the save's "
       "planet, back into the filter")
    handed, good = [], True
    for i in range(60):
        mem = dict(image)
        mem[(flag, 1)] = 1
        cpu = Ppc(code, cave, {})
        for (a, n), v in mem.items():
            cpu.st(a, v, n)
        cpu.r[3], cpu.r[7], cpu.r[29], cpu.r[6] = 0x77, i, 4, 0x00DA5A94 + 4 * i
        before = dict(cpu.mem)
        how = cpu.run(stub_at["mh_map_list_stub"])
        if i < len(want):
            handed.append(cpu.r[3])
            good = good and how == ("b", hdr["GAME_MAP_ACCEPT"]) and cpu.crf.get(1) == 1
        else:
            good = good and how == ("blr", None) and cpu.r[3] == 0 and cpu.crf.get(1) == -1
        good = good and cpu.r[6] == 0x00DA5A94 + 4 * i and cpu.written <= map_ok and cpu.mem == before
    ok(good and handed == want,
       "map list stub, SH_MAP up: r3 the r7th of the Trainer's %d planets, past the filter to 0x%08X with cr1 as "
       "cmpwi cr1,r29,0 leaves it; past them 0, back into the filter, which skips it; nothing written but r3, "
       "r12, cr0 and cr1" % (len(want), hdr["GAME_MAP_ACCEPT"]))


def f32(bits):
    return struct.unpack(">f", struct.pack(">I", bits))[0]


def f32_bits(x):
    return struct.unpack(">I", struct.pack(">f", x))[0]


def imm32(elf, ori_at):
    """The constant a `lis rS,hi` ... `ori rA,rS,lo` pair builds, by the ori's address (the lis up to 16 words before)."""
    w = elf.word(ori_at)
    if w is None or (w >> 26) != 24:
        return None
    rs = (w >> 21) & 31
    for k in range(1, 17):
        p = elf.word(ori_at - 4 * k)
        if p is not None and (p >> 26) == 15 and ((p >> 16) & 31) == 0 and ((p >> 21) & 31) == rs:
            return ((p & 0xFFFF) << 16) | (w & 0xFFFF)
    return None


def check_rac3_av(ok, elf, hdr, code, cave):
    """
    4b for UYA: the CONFIG page is Options' AUDIO / VISUAL screen rebuilt (its
    getter 0x2DB6B8, init 0x2DD218, grid 0x2DC6F0, sliders 0x2DBE70): each of
    the header's AV_* numbers is the one that screen uses, read back from where
    it uses it; the grid configuration in the shim is that screen's; the font,
    the fit and the grid's fields the shim relies on are the game's; and the
    handles the shim adds are nobody else's.
    """
    W = elf.word
    # The numbers: a float word in the executable, or the constant a lis/ori pair builds.
    names = sorted(k for k in hdr if k.startswith("AV_") and not k.endswith("#comment"))
    bad = []
    computed = {"AV_COL1_W"}
    for name in names:
        comment = hdr.get(name + "#comment", "")
        m = re.search(r"(0x[0-9A-Fa-f]+)\s*$", comment)
        if name in computed or not m:
            continue
        at = int(m.group(1), 16)
        got = imm32(elf, at) if "lis/ori" in comment else W(at)
        if got != hdr[name]:
            bad.append("%s 0x%08X: 0x%08X at 0x%08X" % (name, hdr[name], got or 0, at))
    ok(not bad, "AUDIO / VISUAL's numbers in the header are the screen's own: its list's floats (0xC4FBE0..), its "
       "frame, cursor bar and rules (0x2DC6C8.., 0x2DC730..), its panel (0xC4FCC0) and its sliders (0x2DD1E0.., "
       "0x2DBE60..)")
    for b in bad[:12]:
        print("        " + b)
    col1 = f32_bits(f32(f32_bits(f32(hdr["AV_COL1_X"]) - f32(hdr["AV_COL0_X"]))) - f32(hdr["AV_COL0_W"]))
    ok(hdr["AV_COL1_W"] == col1 and W(0x002DCA38) == 0xEC241828 and W(0x002DCA68) == 0xD02100BC,
       "the values' box, 0.885 - 0.115 - 0.53 in single precision, as 0x2DCA38 subtracts it (0x%08X)" % col1)

    # The colours, by the instructions that build them.
    colours = [
        ("AV_KNOB_COLOR", 0x002DD55C, "CreateRoundedRect of a knob, 0x1E0009"),
        ("AV_TRACK_COLOR", 0x002DD5CC, "of a track, 0x1E0008"),
        ("AV_TRACK_COLOR", 0x002DD610, "CreateRect of a groove, 0x1E0007"),
        ("AV_TRACK_COLOR2", 0x002DD814, "then the track's SetColor"),
    ]
    good = all(imm32(elf, at) == hdr[n] for n, at, _ in colours) and \
        W(0x002DD824) == (0x3C800000 | (hdr["AV_GROOVE_COLOR2"] >> 16)) and hdr["AV_GROOVE_COLOR2"] & 0xFFFF == 0 and \
        W(0x002DD630) == 0x38800001 and W(0x002DD638) == 0x38A00003 and \
        branch_target(0x002DD63C, W(0x002DD63C)) == 0x002ADAC8 and \
        W(0x00C4FCCC) == hdr["AV_PANEL_COLOR"]
    ok(good, "the sliders' colours as AUDIO / VISUAL creates and sets them (0x2DD554 .. 0x2DD82C), the groove "
       "anchored left (SetAnchor(1, 3), 0x2DD63C), and the panel's from its widget table")

    # The grid configuration, word by word, as 0x2DC6F0 builds it on the stack at r1+0x84.
    m = re.search(r"static const u32 cfg_template\[24\] = \{(.*?)\};",
                  open(os.path.join(HERE, "mh_rac3.c"), encoding="utf-8").read(), re.S)
    tpl = []
    if m:
        for tok in m.group(1).replace("\n", " ").split(","):
            tok = tok.strip()
            if tok:
                tpl.append(hdr[tok] if tok in hdr else int(tok, 0))
    sites = [
        (0x002DC9D0, 0x38E00014), (0x002DCA10, 0x90E1008C),            # [2] layer 20
        (0x002DCA14, 0x38C00002), (0x002DCA2C, 0x90C10098),            # [5] two columns
        (0x002DCA20, 0x3CA03F80), (0x002DCA34, 0x90A1009C),            # [6] scale 1.0
        (0x002DCA00, 0xD06100A0), (0x002DCA08, 0xD0A100A4),            # [7] [8] from 0xC4FBE0, 0xC4FBE8
        (0x002DCA30, 0xD02100A8), (0x002DCA40, 0xD04100AC),            # [9] [10] from 0xC4FBEC, 0xC4FBF0
        (0x002DCA44, 0x90A100B0), (0x002DCA48, 0x90A100B4),            # [11] [12] 1.0
        (0x002DCA4C, 0xD06100B8),                                      # [13] from 0xC4FBF4
        (0x002DCA50, 0x93E100C0), (0x002DCA54, 0x93E100C4), (0x002DCA58, 0x93E100C8),
        (0x002DCA5C, 0x93E100CC), (0x002DCA64, 0x93E100D0), (0x002DCA6C, 0x93E100D4),   # [15..20] 0
        (0x002DCA7C, 0x3081 << 16 | 0x0084),                           # GridBuild(g, r1+0x84)
    ]
    want = [None, None, 0x14, None, None, 2, 0x3F800000, W(0x00C4FBE0), W(0x00C4FBE8), W(0x00C4FBEC),
            W(0x00C4FBF0), 0x3F800000, 0x3F800000, W(0x00C4FBF4), col1, 0, 0, 0, 0, 0, 0, 0, 0, 0]
    ok(len(tpl) == 24 and all(want[i] is None or tpl[i] == want[i] for i in range(24)) and
       tpl[0] == hdr["U_CELLS_SIZE"] and all(W(a) == w for a, w in sites),
       "the shim's grid configuration is AUDIO / VISUAL's (0x2DC6F0, r1+0x84) but for the buffer, its size and "
       "the rows; words 21..23, which GridBuild does not read, 0")

    # The calls 0x2DC6F0 makes on the grid after GridBuild, with the shim's arguments.
    calls = [
        (0x002DCAC0, 0x002D5BF4, "SetFrameRect(0.5, 0.8173, 0.045, 0.0425, 0.35)"),
        (0x002DCAE8, 0x002D5C14, "SetCursorBar(0.5, 0.6322, 0.8173, 0.4391)"),
        (0x002DCB1C, 0x002D5BE0, "the rules(on, 0.027, 0.00666, 0.03)"),
        (0x002DCB40, 0x002D76E0, "column 0 aligned (1, 3)"),
        (0x002DCB64, 0x002D76E0, "column 1 aligned (2, 3)"),
        (0x002DCDFC, 0x002D7784, "column 0's cells in font 1"),
        (0x002DCE1C, 0x002D7784, "column 1's"),
    ]
    good = all(branch_target(a, W(a)) == t for a, t, _ in calls) and \
        W(0x002DCAAC) == 0xC39E000C and W(0x002DCAB0) == 0xC3BE0010 and W(0x002DCAB4) == 0xC07E0008 and \
        W(0x002DCAD8) == 0xC0210074 and W(0x002DCADC) == 0xC0410078 and W(0x002DCAE0) == 0xC061007C and \
        W(0x002DCAE4) == 0xC0810080 and W(0x002DCB04) == 0xC3DE0014 and W(0x002DCB08) == 0x38800001 and \
        W(0x002DCB0C) == 0xC3FE0018 and W(0x002DCB18) == 0xC025FBE4 and \
        W(0x002DCB34) == 0x38800000 and W(0x002DCB38) == 0x38A00001 and W(0x002DCB3C) == 0x38C00003 and \
        W(0x002DCB58) == 0x38800001 and W(0x002DCB5C) == 0x38A00002 and W(0x002DCB60) == 0x38C00003 and \
        W(0x002DCDF4) == 0x38800000 and W(0x002DCDF8) == 0x38A00001 and \
        W(0x002DCE14) == 0x38800001 and W(0x002DCE18) == 0x38A00001
    ok(good, "and AUDIO / VISUAL's calls after GridBuild are the shim's: " + "; ".join(c for _, _, c in calls))

    # The font: 0x2D7784 sets each cell's through 0x2AE898, which for a text widget
    # is 0xA80120 (registered at 0xBDC908 in the map 0x1356C14), +0x58; the text's
    # rectangle and draw (0x2B6128) take 1 there as font 1.
    ok(W(0x00BDC908) == 0x00A80120 and W(0x00A80170) == 0x93FE0058 and W(0x002AE958) == 0x30656C14 and
       W(0x002B615C) == 0x80630058 and W(0x002B6194) == 0x38600001 and
       branch_target(0x002B6198, W(0x002B6198)) == 0x0013777C and hdr["U_FONT_MENU"] == 1,
       "font 1: the text widget's +0x58 (0xA80120, through 0x2AE898), which 0x2B6128 hands 0x13777C as 1")
    # The fit: 0x2B0514 sets a text's box (+0x48 width, +0x4C least scale), 0x2B06C4 its flag 1,
    # with which 0x2B6128 scales it by 0x13AD78(least, text, width).
    ok(W(0x002B62D0) == 0x887D0044 and W(0x002B6300) == 0xC03D0048 and W(0x002B6310) == 0xC03D004C and
       branch_target(0x002B6324, W(0x002B6324)) == 0x0013AD78,
       "the fit: a text with flag 1 is scaled to its box, +0x48 wide, no smaller than +0x4C (0x2B62D0 .. 0x2B6324)")
    # The grid's fields the sliders follow.
    ok(W(0x002D69C8) == 0x80830004 and W(0x002D74CC) == 0x80A3001C and W(0x002D7538) == 0xC0630058 and
       W(0x002D7540) == 0xD0230058 and W(0x002D7498) == 0x90E3001C and W(0x002D74A0) == 0x90E30058 and
       W(0x002D74AC) == 0x90830004 and hdr["U_GRID_TOP"] == 4 and hdr["U_GRID_SCROLL"] == 0x1C and
       hdr["U_GRID_SLIDE"] == 0x58,
       "the grid's top row at +4 (GridCursor 0x2D69C8), its scroll at +0x1C and the slide at +0x58, which "
       "0x2D73E8 zeroes as the rows stop")
    # The panel: AUDIO / VISUAL's widget table, and the colour 4 the grey a row of colour 1 is drawn in.
    ok(W(0x00C4FC60) == hdr["AV_ROOT"] and W(0x00C4FC90) == hdr["AV_PANEL"] and W(0x00C4FC94) == hdr["AV_ROOT"] and
       W(0x00C4FCC0) == hdr["AV_PANEL_BOX"] and W(0x00C4FCC4) == hdr["AV_PANEL"] and W(0x00C4FCC8) == 3 and
       W(0x00C4FCD0) == hdr["AV_FRAME_X"] and W(0x00C4FCF0) == hdr["AV_PIC"] and W(0x00C4FCF4) == hdr["AV_PANEL"] and
       W(0x00C4FCF8) == 2 and W(0x00C4FD14) == 0 and W(0x00C4FD10) == 0 and W(0x00C4FD1C) == 0 and
       W(0x002DCFD0) == 0xC0DE0028 and W(0x002DCFD4) == 0xFCA03090 and
       branch_target(0x002DCFD8, W(0x002DCFD8)) == 0x002B1A88 and
       W(0x002DD43C) == 0x38600011 and W(0x002DD440) == 0x3C80001E and
       branch_target(0x002DD444, W(0x002DD444)) == 0x002AF6C4,
       "the panel: the root 0x1E0000 on layer 17, its container and rounded box (type 3, both corners the "
       "table's +0x28) as AUDIO / VISUAL's table has them; 0x1E0003, its picture, a rectangle (type 2) in the "
       "panel's container, no texture, no anchor and no flags of its own (round 4: the shim's too)")
    check_rac3_pic(ok, elf, hdr)
    ok(W(0x002D6EF4) == 0x80850090 and W(0x002DE1F0) == 0x38600011,
       "a row of colour 4 draws in the grey +0x90; AUDIO / VISUAL's deinit clears layer 17 (and 16, 20) as the "
       "shim's does")
    # The main pause's frame, which a MAIN row shrinks to fit.
    ok(W(0x002F4144) == 0x3E840B78 and W(0x002F4150) == 0x3F3F3B64 and W(0x002F4138) == 0x3EDC28F6 and
       f32(hdr["U_ROW_FIT_W"]) < f32(0x3F3F3B64) - f32(0x3E840B78),
       "a MAIN row's box, 0.44, is inside the main pause's end pieces at 0.258 and 0.747")

    # The handles: 0x1E0001 .. 0x1E000F only AUDIO / VISUAL's code builds; the shim's slots nobody.
    lis, built = {}, []
    tv, tfs, tms, toff, _ = elf.text
    for a, w in elf.words(elf.text):
        op, rd, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        if op == 15 and ra == 0:
            lis[rd] = (w & 0xFFFF, a)
        elif op == 24 and rd in lis and a - lis[rd][1] < 256:
            v = (lis[rd][0] << 16) | (w & 0xFFFF)
            if (v >> 16) == 0x1E and (v & 0xFFFF) < 0x100:
                built.append((a, v))
    slots_hi = hdr["AV_SLOT0"] + 3 * hdr["U_GRID_VISIBLE"]
    outside = [(a, v) for a, v in built if 0x001E0001 <= v <= 0x001E000F and not 0x002DB6B8 <= a < 0x002DE900]
    ours = [(a, v) for a, v in built if hdr["AV_SLOT0"] <= v < slots_hi]
    ok(built and not outside and not ours and hdr["AV_SLOT0"] > 0x001E0010 and slots_hi <= 0x001E0100,
       "the handles 0x1E0001..0x1E000F are built only in AUDIO / VISUAL's code (0x2DB6B8..0x2DE900), and the "
       "slider slots' 0x%08X..0x%08X by no code in the executable" % (hdr["AV_SLOT0"], slots_hi - 1))
    for a, v in (outside + ours)[:12]:
        print("        0x%08X: 0x%08X" % (a, v))


def check_rac3_pic(ok, elf, hdr):
    """
    4b for UYA, round 4: the picture in the panel is AUDIO / VISUAL's own, set
    up, shown and taken down the way that screen does it (its init 0x2DD218,
    update 0x2DC0B0, deinit 0x2DE1D8): the header's source, the hud.wad word,
    the picture set, the row whose picture it is, the wait, and the vtable
    entry the shim calls rather than reading the vtable at run time.
    """
    W = elf.word

    def bl(a, t):
        return branch_target(a, W(a)) == t and (W(a) >> 26) == 18 and (W(a) & 3) == 1

    def lis(w):
        return (w & 0xFFFF) << 16

    def simm(w):
        v = w & 0xFFFF
        return v - 0x10000 if v & 0x8000 else v

    # Its init: two buffers, then source 1 with the hud.wad's word and the picture set.
    wad = (lis(W(0x002DD350)) + simm(W(0x002DD358)) + simm(W(0x002DD36C))) & 0xFFFFFFFF
    pset = (lis(W(0x002DD354)) + simm(W(0x002DD360))) & 0xFFFFFFFF
    ok(W(0x002DD31C) == 0x38600000 and bl(0x002DD320, 0x002D3A4C) and W(0x002DD330) == 0x38600000 and
       bl(0x002DD334, 0x002D3A4C) and W(0x002DD344) == 0x38600000 | hdr["U_PIC_SOURCE"] and
       bl(0x002DD348, 0x002CCA44) and W(0x002DD350) == 0x3C8000D6 and W(0x002DD358) == 0x30C4C610 and
       W(0x002DD36C) == 0x80C64964 and W(0x002DD354) == 0x3CE000D6 and W(0x002DD360) == 0x30E73550 and
       bl(0x002DD370, 0x002CCD48) and wad == hdr["U_PIC_HUD_WAD"] and pset == hdr["U_PIC_SET"],
       "AUDIO / VISUAL's init takes two picture buffers (0x2D3A4C(0)) and sets picture source %d up with them, "
       "the hud.wad's word 0x%08X and its picture set 0x%08X (0x2CCD48), as the shim does" %
       (hdr["U_PIC_SOURCE"], wad, pset))
    # Its end: the rectangle tied to the source, picture 0, hidden, ten frames' fade.
    ok(W(0x002DD998) == 0x38600000 | hdr["U_PIC_SOURCE"] and bl(0x002DD99C, 0x002CCA44) and
       W(0x002DD9A8) == 0x3C60001E and W(0x002DD9AC) == 0x60630003 and bl(0x002DD9B0, 0x002AFF84) and
       W(0x002DD9BC) == 0x38800000 and bl(0x002DD9C4, 0x002AE710) and W(0x002DD9D0) == 0x38800000 and
       bl(0x002DD9D8, 0x002ADDF8) and W(0x002DD9E4) == 0x38800001 and W(0x002DD9EC) == 0x38A0000A and
       bl(0x002DD9F0, 0x002B0040) and hdr["AV_PIC"] == 0x001E0003,
       "then ties 0x1E0003 to the source (0x2AFF84), shows picture 0 (0x2AE710), hides it and gives it a fade "
       "of ten frames (0x2B0040), as the shim's pic_tie_up")
    # Its update: once the cursor has rested 30 frames, the row's picture, shown.
    rows, row = hdr["U_PIC_ROWS"], hdr["U_PIC_ROW"]
    ok(W(0x002DC610) == 0x38A00000 | hdr["U_PIC_WAIT"] and W(0x002DC614) == 0x90A3FE20 and
       W(0x002DC62C) == 0x3063FFFF and lis(W(0x002DC640)) + simm(W(0x002DC648)) == rows + 8 and
       bl(0x002DC664, 0x002AE710) and W(0x002DC670) == 0x38800001 and bl(0x002DC678, 0x002ADDF8) and
       W(rows + 12 * row) == 0x233 and W(rows + 12 * row + 8) == hdr["U_PICTURE"],
       "its update shows a row's picture (0x2AE710, then SetVisibility 1) %d frames after the cursor rests; "
       "row %d of its table, EFFECTS VOLUME (0x233), has picture %d, the user's" %
       (hdr["U_PIC_WAIT"], row, hdr["U_PICTURE"]))
    # Its deinit: the source's vtable's second entry, the buffers back, the disc waited for.
    vt = None
    if W(0x002CCA58) == 0x3CA000B2 and W(0x002CCA6C) == 0x30A5C630 and W(0x002CCA78) == 0x90A40000:
        vt = (lis(W(0x002CCA58)) + simm(W(0x002CCA6C))) & 0xFFFFFFFF
    ok(W(0x002DE21C) == 0x38600000 | hdr["U_PIC_SOURCE"] and bl(0x002DE220, 0x002CCA44) and
       W(0x002DE228) == 0x80830000 and W(0x002DE230) == 0x80840004 and W(0x002DE240) == 0x4E9E0421 and
       vt == 0x00B1C630 and W(vt + 4) == hdr["OPD_PIC_SOURCE_STOP"] and
       bl(0x002DE250, 0x002D3B94) and bl(0x002DE260, 0x002D3B94) and W(0x002DE268) == 0x38600001 and
       bl(0x002DE26C, 0x00234CA8),
       "its deinit calls the source's vtable's second entry (every source is made with vtable 0x%08X, whose "
       "second entry is OPD_PIC_SOURCE_STOP), gives both buffers back (0x2D3B94) and waits for the disc's reads "
       "(0x234CA8(1)), as the shim's pic_close" % (vt or 0))


def map_list(hdr):
    """MH_MAP_LIST, the Trainer's GALACTIC MAP's planets, out of the header."""
    text = open(os.path.join(HERE, "mh_rac3.h"), encoding="utf-8").read()
    m = re.search(r"#define\s+MH_MAP_LIST\s+([0-9, ]+)", text)
    return [int(x) for x in m.group(1).split(",")] if m else []


def rac3_planet_names():
    """PLANET_LIST's names for UYA, out of src/games/rac3.c (rac3_planets)."""
    text = open(os.path.join(QWARK, "src", "games", "rac3.c"), encoding="utf-8").read()
    m = re.search(r"static const char \* const rac3_planets\[\] = \{(.*?)\};", text, re.S)
    return re.findall(r'"([^"]*)"', m.group(1)) if m else []


def check_rac3_map(ok, elf, hdr, code, cave, data, stub_at):
    """
    4c for UYA: the GALACTIC MAP the Planets row opens, and the parts of it the
    shim relies on: its GuiState's descriptors, the list build's loop round the
    site, the list's entries, count and cursor, the exits of its update the
    shim takes first, a picture for every planet the Trainer lists, the name
    words, and the Trainer's list itself.
    """
    W = elf.word
    ok(W(0x00BDD938) == 0x002CED28 and W(0x00BDD940) == 0x002CFB60 and W(0x00BDD948) == 0x002D0600 and
       W(0x00BDD950) == 0x002D0744 and W(0x00BDD958) == 0x002D0768 and
       all(W(a + 4) == UYA_TOC for a in range(0x00BDD938, 0x00BDD960, 8)),
       "the map's getter 0x2CE130 hands its GuiState these five: init 0x2CED28, update 0x2CFB60, deinit "
       "0x2D0600, pre 0x2D0744, post 0x2D0768 (OPD_MAP_*, U_DESC_MAP_*)")
    ok(W(0x002CE134) is not None and
       [a for a, w in elf.words(elf.text) if (w >> 26) == 18 and (w & 3) == 1 and branch_target(a, w) == 0x002CE7B8] ==
       [0x002CEE7C],
       "the list build 0x2CE7B8 has one caller, the map's init at 0x2CEE7C: SH_MAP up is that init")
    ctx = [
        (0x002CE824, 0x30C65A94, "the build's loop: r6 = 0xDA5A94, the save's list ..."),
        (0x002CE828, 0x63A70000, "... r7 = 0, the index, ..."),
        (0x002CE834, 0x80660000, "... r3 = its r7th word, ..."),
        (0x002CE83C, 0x2C030000, "... then cmpwi r3,0 after the site, which sets cr0 again ..."),
        (0x002CE840, 0x4C402342, "... crorc eq,lt,4*cr1+lt, which reads the site's cr1 ..."),
        (0x002CE844, 0x418200A0, "... beq 0x2CE8E4, the skip ..."),
        (0x002CE848, 0x2C030000, "cmpwi r3,0; beq skip: the stub's 0 is skipped as the game's"),
        (0x002CE84C, 0x41820098, None),
        (0x002CE88C, 0x2C9D0000, "cmpwi cr1,r29,0, which the stub sets as this does ..."),
        (0x002CE894, 0x40840010, "... for the bge cr1 at GAME_MAP_ACCEPT, the entry made from r3:"),
        (0x002CE8D0, 0x7C7E412E, "stwx r3,r30,r8, the entry's planet"),
        (0x002CE8D4, 0x93BBF0A0, "stw r29,-0xF60(r27), the count 0xC4F0A0"),
        (0x002CE8E4, 0x30E70001, "the skip: r7 + 1 ..."),
        (0x002CE8EC, 0x2C07003C, "... to sixty"),
        (0x002CE7E8, 0x33DE75B8, "the entries at 0x13775B8 (lis r30,0x137 before)"),
        (0x002CFBE8, 0x808400C4, "the map's update reads the pad's presses, +0xC4, ..."),
        (0x002CFBEC, 0x80A5A7F0, "... the confirm mask 0xCBA7F0 ..."),
        (0x002CFBF0, 0x9066F098, "... and keeps the grid's cursor in 0xC4F098 (U_MAP_CURSOR), which X reads"),
        (0x002CFC94, 0x54830630, "Square: bit 0x80"),
        (0x002CFD44, 0x54A406F6, "Triangle: bit 0x10"),
        (0x002D07D0, 0x38600014, "the pick's travel sound 0x14 (SND_MAP_GO)"),
    ]
    for a, want, what in ctx:
        ok(W(a) == want, "0x%08X holds 0x%08X: %s" % (a, want, what or "the same"))
    exits = [(0x002CFC5C, "the cross: bctrl, the entry's callback 0x2D078C (the travel setter, then out)"),
             (0x002CFCB4, "Square: bl MISSIONS' getter 0x2F5758"),
             (0x002CFD80, "Triangle on planets 12 and 18: bl BATTLE MISSIONS' getter 0x2DECB4"),
             (0x002CFDA4, "Triangle elsewhere: bl the travel setter 0x2D22DC, a reload"),
             (0x002D0514, "the Start close: bl 0x2D21C8, the request the shim takes first")]
    targets = {0x002CFCB4: 0x002F5758, 0x002CFD80: 0x002DECB4, 0x002CFDA4: 0x002D22DC, 0x002D0514: 0x002D21C8}
    for a, what in exits:
        w = W(a)
        good = w == 0x4E9E0421 if a == 0x002CFC5C else (w >> 26) == 18 and branch_target(a, w) == targets[a]
        ok(good, "the map's update, 0x%08X: %s" % (a, what))
    ok(W(0x00BDD960) == 0x002D078C, "each entry's callback 0xBDD960 is the pick's, 0x2D078C")
    # The MISSIONS hint the shim hides: 0x130028, made in the init with text 0x15C6.
    init = [W(a) for a in range(0x002CED28, 0x002CF600, 4)]
    ok(0x386015C6 in init and any(imm32(elf, a) == hdr["U_MAP_HINT"] for a in range(0x002CED28, 0x002CF600, 4)
                                  if (W(a) >> 26) == 24),
       "the init makes Square's hint, 0x%08X, with text 0x15C6 (\" MISSIONS\"), which the Trainer's map "
       "relabels VID-COMICS" % hdr["U_MAP_HINT"])

    # The Trainer's list: rac3.c's menu order, the planets with a picture.
    want = map_list(hdr)
    rac3 = open(os.path.join(QWARK, "src", "games", "rac3.c"), encoding="utf-8").read()
    m = re.search(r"rac3_planet_order\[\] = \{(.*?)\};", rac3, re.S)
    order = [int(x) for x in re.findall(r"\b(\d+)\b", re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S))] if m else []
    m = re.search(r"rac3_planets\[\] = \{(.*?)\};", rac3, re.S)
    names = re.findall(r'"([^"]*)"', m.group(1)) if m else []
    pics = dict((p, W(0x00C4EFFC + 4 * p)) for p in range(0, 40))
    rv, rfs, rms, _, _ = elf.rw
    with_pic = [p for p in order if 1 <= p < 30 and p < len(names) and not names[p].startswith("(") and
                rv <= (pics[p] or 0) < rv + rms]
    ok(want and want == with_pic and len(want) == hdr["MH_MAP_LIST_LEN"] and len(want) <= 30 and
       15 not in want and 25 not in want,
       "the Trainer's list, %d planets, is rac3.c's menu order less the gaps and the placeholder and those the "
       "map has no picture for (the vid comics 30..36: 0xC4EFFC[p] past 29 is the map's own variables), at most "
       "the map's 30" % len(want))
    ok(all((pics[p] or 0) != 0 for p in range(1, 30)) and pics[30] == 0xFFFFFFFF,
       "0xC4EFFC has a picture for every planet 1 .. 29; at 30 it is 0xC4F074, the map's own -1")
    # The names (the pickers' round 2): the build's rule (0x2CEB04: the planet's text for these, the location's
    # for the rest) and the init's (the same), both of which mh_map_init answers by swapping both words of each
    # listed planet for MH_MSG_PLANET + p, which the msg stub answers with PLANET_LIST's name.
    group = set(w & 0xFFFF for w in (W(a) for a in range(0x002CEB04, 0x002CEB74, 4)) if (w >> 16) == 0x2C03)
    ok(group == {1, 2, 4, 5, 8, 9, 10, 13, 14, 18, 23, 27, 28, 29},
       "the build names planets %s by their planet's text, the rest by their location's" % sorted(group))
    ok(W(0x002CEA84) == 0x33390000 | 0x09E4 and W(0x002CEA78) == 0x3F2000C4 and W(0x002CEBB4) == 0x80830004 and
       W(0x002CEBFC) == 0x7CB9182E and branch_target(0x002CEC08, W(0x002CEC08)) == 0x0014BCF0,
       "the build reads a planet's text id out of the name table 0xC409E4 (r25) and hands it to msg_string "
       "(0x14BCF0), whose first word is the msg stub's")
    table = dict(((0x00C409E4 + 8 * p + 4 * k), W(0x00C409E4 + 8 * p + 4 * k)) for p in range(30) for k in (0, 1))
    shown = [names[p] if p < len(names) else None for p in want]
    before = [table[0x00C409E4 + 8 * p + (4 if p in group else 0)] for p in want]
    ok(all(p < hdr["MH_MAP_PLANETS"] for p in want) and None not in shown and
       not [n for n in shown if n.startswith("(")] and len(set(shown)) == len(want) and
       all(table[0x00C409E4 + 8 * p] != 0 or table[0x00C409E4 + 8 * p + 4] != 0 for p in want),
       "with them every one of the %d entries reads as the client's planet picker names it, PLANET_LIST's name, "
       "each its own (the game's own texts would repeat %d), every one of their name words a text of the game's "
       "to go back" % (len(want), len(before) - len(set(before))))
    ok(b"\x13 VID-COMICS\x00" in data,
       "Square's hint in the Trainer's map, \"\x13 VID-COMICS\", is in the data image")


def check_rac3(ok, elf, roots):
    hdr = header_defs(os.path.join(HERE, "mh_rac3.h"))
    box_defs = header_defs(MENUBOX)
    shim_defs = header_defs(SHIM_H)
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
    cl, ch = hdr["U_CELLS"], hdr["U_CELLS_END"]
    for lo, hi, what in ((mbx, mbx_end, "mailbox"), (dat, dat_end, "data"), (cl, ch, "cells")):
        ok(lo >= rv + rms and hi <= page_end(rv + rms),
           "%s area 0x%08X..0x%08X is past the read-write segment's end (0x%08X), in its last page" %
           (what, lo, hi, rv + rms))
    for lo, hi, what in ((cave, cave_end, "code"), (mbx, mbx_end, "mailbox"), (dat, dat_end, "data"),
                         (cl, ch, "cells")):
        br, wd, pr = refs_into(elf, lo, hi)
        ok(not wd and not pr, "no word and no lis/addi pair in the executable names the %s area%s" %
           (what, "" if not br else " (%d data words decode as branches into it)" % len(br)))

    # 7. the mailbox and the data image
    magic, version, size = struct.unpack(">IHH", box[0:8])
    ok(len(box) == hdr["MH_MBOX_SIZE"] and magic == box_defs["MBOX_MAGIC"] and
       version == box_defs["MBOX_VERSION"] and size == hdr["MH_MBOX_SIZE"],
       "the mailbox image: 'QMNU', version %d, %d bytes" % (version, size))
    ok(box[8] == hdr["MH_ROWS"] == 24 and box[9] == hdr["MH_ROW_BYTES"] == box_defs["MBOX_ROW_SIZE"] and
       box[10] == hdr["MH_MAIN_ROWS"] == 9 and box[11] == hdr["MH_CAPS"] and box[12] == 0 and box[13] == 0 and
       box[14] == 3 and box[15] == hdr["MH_CFG_ROWS"] == 24,
       "row_cap 24, rows of 0x40 (round 4: no descriptions), 9 rows a MAIN page, 24 a CONFIG page, caps 0x%02X, "
       "closed, no request, renderer 3" % box[11])
    ok(box_defs["MBOX_OFF_ROWS"] + hdr["MH_ROWS"] * hdr["MH_ROW_BYTES"] == hdr["MH_MBOX_SIZE"] and
       (hdr["MH_CAPS"] & box_defs["MBOX_CAP_PICKER"]) != 0 and all(b == 0 for b in box[16:]),
       "the rows fill the image, everything after the header zero; caps has the planet picker (MBOX_CAP_PICKER)")
    shim_off = hdr["MH_SHIM"] - hdr["MH_STATE"]
    ok(struct.unpack(">I", data[0:4])[0] == hdr["MH_MAGIC"] and
       all(b == 0 for b in data[4:shim_off + hdr["SH_SIZE"]]),
       "the data image: the core's state at MH_STATE with its magic 'QMH3', the shim's at MH_SHIM "
       "(+0x%X), everything else zero" % shim_off)
    ok(shim_off + hdr["SH_SIZE"] <= len(data), "the shim's state, %d bytes, is all in the image" % hdr["SH_SIZE"])
    cells, cells_end = hdr["U_CELLS"], hdr["U_CELLS_END"]
    ok(hdr["MH_CFG_ROWS"] * hdr["U_GRID_COLS"] * hdr["U_CELL_SIZE"] < hdr["U_CELLS_SIZE"] and
       cells + hdr["U_CELLS_SIZE"] <= cells_end and cells % 4 == 0 and
       (cells_end <= mbx or cells >= mbx_end) and (cells_end <= dat or cells >= dat_end),
       "the grid's cells (round 4: a span of their own), %d bytes at 0x%08X: more than a page of %d rows of two "
       "cells of 68 (GridBuild builds nothing in a buffer that is not), clear of the mailbox and the data" %
       (hdr["U_CELLS_SIZE"], cells, hdr["MH_CFG_ROWS"]))

    # 1. the originals, 2. the hook words
    order = ["MH_SITE_MSG", "MH_SITE_ROWS8", "MH_SITE_ROWS9A", "MH_SITE_ROWS9B", "MH_SITE_INJECT",
             "MH_SITE_COMBO", "MH_SITE_LAUNCH", "MH_SITE_MAP_LIST", "MH_SITE_HELP8_TEXT", "MH_SITE_HELP8_CB",
             "MH_SITE_HELP9A_TEXT", "MH_SITE_HELP9A_CB", "MH_SITE_HELP9B_TEXT", "MH_SITE_HELP9B_CB"]
    ok(len(hooks) == 14 and [h[0] for h in hooks] == [hdr[k] for k in order],
       "fourteen words: the label hook, the three row counts, the Start path's three, the GALACTIC MAP's list "
       "word, then the HELP rows' six")
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

    # 4b. what the CONFIG page takes from the game: AUDIO / VISUAL's look
    check_rac3_av(ok, elf, hdr, code, cave)

    # 3. the descriptors the game calls
    if help_desc is not None:
        c = struct.unpack(">I", data[help_desc - dat:help_desc - dat + 4])[0]
        ok(c != 0, "mh_help is code at 0x%08X" % c)

    # 4. the code, 5. the descriptors
    allowed = (hdr["GAME_MSG_EXIT"], hdr["GAME_MAP_ACCEPT"])
    bad = lint_code(code, cave, allowed=allowed)
    ok(not bad, "the code: no branch out but the label stub's and the map list stub's, no sc, no vector "
       "instruction, no write to r13")
    for b in bad[:12]:
        print("        " + b)
    outs = [(cave + i, branch_target(cave + i, struct.unpack(">I", code[i:i + 4])[0])) for i in range(0, len(code), 4)
            if branch_target(cave + i, struct.unpack(">I", code[i:i + 4])[0]) in allowed]
    ok(sorted(t for _, t in outs) == sorted(allowed) and
       all((struct.unpack(">I", code[o - cave:o - cave + 4])[0] & 3) == 0 for o, _ in outs) and
       all(stub_at.get(n, -1) <= o < stub_at.get(n, -1) + 0x80 for (o, t), n in
           zip(sorted(outs, key=lambda x: x[1] == hdr["GAME_MAP_ACCEPT"]), ("mh_msg_stub", "mh_map_list_stub"))),
       "exactly two ways out, plain branches: the label stub on into msg_string, the map list stub on past the "
       "build's filter (0x%08X)" % hdr["GAME_MAP_ACCEPT"])
    launch = stub_at.get("mh_launch_stub")
    if launch is not None:
        lw = [struct.unpack(">I", code[launch - cave + 4 * i:launch - cave + 4 * i + 4])[0] for i in range(13)]
        t = branch_target(launch + 24, lw[6])
        ok(lw[1] == 0x7C0802A6 and lw[2] == 0xF8010090 and lw[3] == 0xF8410078 and (lw[6] >> 26) == 18 and
           (lw[6] & 3) == 1 and cave <= t < cave + len(code) and lw[8] == 0xE8410078 and lw[12] == 0x4E800020,
           "the launch stub saves LR and the game's r2, calls mh_launch with the renderer's TOC, puts both "
           "back and returns to the launcher")

    opds = dict((v, k) for k, v in hdr.items() if k.startswith("OPD_") and not k.endswith("#comment"))
    handed = {hdr["U_DESC_PRE"], hdr["U_DESC_POST"], hdr["U_DESC_MAP_PRE"], hdr["U_DESC_MAP_POST"]}
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
    for name, fn in (("U_DESC_MAP_PRE", 0x002D0744), ("U_DESC_MAP_POST", 0x002D0768)):
        ok(elf.word(hdr[name]) == fn and elf.word(hdr[name] + 4) == UYA_TOC and
           code_pointers(code, cave, hdr[name], hdr[name] + 8),
           "%s 0x%08X = {0x%08X, TOC}, the GALACTIC MAP's own render, which the shim hands its map's GuiState" %
           (name, hdr[name], fn))

    # 4c. the GALACTIC MAP the Planets row opens
    check_rac3_map(ok, elf, hdr, code, cave, data, stub_at)

    # 8. the leaf stubs, stepped
    if all(k in stub_at for k in ("mh_msg_stub", "mh_inject_stub", "mh_combo_stub", "mh_rows8_stub",
                                  "mh_rows9a_stub", "mh_rows9b_stub", "mh_map_list_stub")):
        try:
            check_rac3_stubs(ok, code, cave, hdr, box_defs, shim_defs, stub_at, data)
        except ValueError as e:
            ok(False, "the leaf stubs step: %s" % e)
    else:
        ok(False, "every leaf stub found to step")

    # 9. overlaps
    ours = [(cave, cave_end, "renderer code span"), (mbx, mbx + len(box), "renderer mailbox"),
            (dat, dat + len(data), "renderer data"), (hdr["U_CELLS"], hdr["U_CELLS_END"], "renderer cells")]
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
             mod_pointers(roots, "NPEA00387", dat, dat_end) +
             mod_pointers(roots, "NPEA00387", hdr["U_CELLS"], hdr["U_CELLS_END"])]
    ok(not named, "no mod's code names an address in any of the four areas")
    for x in named[:12]:
        print("        " + x)
    seen_acc = set()
    for c, mod in accepted:
        if mod not in seen_acc:
            print("        known: %s/...: %s" % (mod, ACCEPTED_RAC3[mod]))
            seen_acc.add(mod)
    print("        (%d words shared with the prototypes by design)" % len(accepted))


# ------------------------------------------------------------- Going Commando

RAC2_TOC = 0x0131D0F0

# qwark's own writes into RaC2, build 54: src/games/rac2*.{c,h},
# src/games/sfhelper/sf_rac2.h, src/games/switches/sw_rac2.ld. (lo, hi, what).
QWARK_RAC2 = [
    (0x0113EC64, 0x01141400, "the savefile helper's cave"),
    (0x01141400, 0x011414FC, "code switches (RPCS3) and the crash guards' caves"),
    (0x00B086B0, 0x00B086B4, "savefile helper hook"),
    (0x01317728, 0x01317730, "savefile helper request bytes"),
    (0x01C00000, 0x01E00000, "savefile aside buffer"),
    (0x00BEA8A0, 0x00BEA8A4, "fast loads, also the fingerprint"),
    (0x00B30C7C, 0x00B30C80, "infinite ammo"),
    (0x00086960, 0x00086964, "the grid crash guard's site"),
    (0x00BDF30C, 0x00BDF310, "the ship crash guard's site"),
    (0x015B3070, 0x015B3071, "the debug mode byte"),
]

# Overlaps that are by design: (mod folder) -> why.
ACCEPTED_RAC2 = {
    "probe-n2": "probe N2 is this renderer's first prototype and hooks the same label, combo and root "
                "words; the install refuses while it is loaded",
    "probe-n2b": "probe N2b is a prototype and hooks the same words but the Start test; the install "
                 "refuses while it is loaded",
    "probe-n2c": "probe N2c is this renderer's prototype and hooks the same words but the Start test; "
                 "the install refuses while it is loaded",
}

# The stubs' first words (mh_rac2_stubs.S), by the symbol in the hook's comment.
STUB_FIRST_RAC2 = {
    "mh_msg_stub": (0xFFFFFFFF, 0x54650032, "rlwinm r5,r3,0,0,25"),
    "mh_inject_stub": (0xFFFFFFFF, 0x54840528, "rlwinm r4,r4,0,20,20, the game's own test"),
    "mh_combo_stub": (0xFFFFFFFF, 0x38A00001, "li r5,1"),
    "mh_root_stub": (0xFFFF0000, 0x3D600000, "lis r11,..."),
}

# The root's seven labels (0x135211C; the fifth, Help's, is the label word) and
# the four label tables of the special roots after it, which have no Help.
R2_ROOT_LABELS = (0x0135211C, (0x2D51, 0x2D52, 0x2D53, 0x2D54, 0x2D55, 0x2D56, 0x2DB3))
R2_OTHER_LABELS = (0x01352138, 0x0135214C, 0x01352160, 0x0135216C)


def check_rac2_stubs(ok, code, cave, hdr, box_defs, shim_defs, stub_at, data):
    shim, state, box = hdr["MH_SHIM"], hdr["MH_STATE"], hdr["MH_BOX"]
    pressed, force = shim + hdr["SH_PRESSED"], shim + hdr["SH_FORCE"]
    request = box + box_defs["MBOX_OFF_REQUEST"]
    opens = state + shim_defs["ST_OPENS"]
    front = state + shim_defs["ST_FRONT"]
    text0, set_size = shim_set_layout(shim_defs, hdr["MH_ROWS"])
    text0 += state
    text_len = shim_defs["MH_TEXT_LEN"]
    screens = hdr["MH_SCREENS"]

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

    # The label hook: Trainer, the rows of the set on show, and everything else on into msg_string.
    msg_ok = {"r3", "r4", "r5", "cr0"}
    cpu, how = run("mh_msg_stub", {3: hdr["MH_MSG_HELP"], 4: 0x1234})
    ok(how == ("blr", None) and cstr(cpu.r[3]) == hdr["MH_ENTRY_LABEL"] == "Trainer" and cpu.written <= msg_ok,
       "msg stub, 0xF000: \"%s\", in the data image, back to msg_string's caller" % hdr["MH_ENTRY_LABEL"])
    for f in (0, 1):
        good = True
        for i in range(hdr["MH_MAIN_ROWS"]):
            cpu, how = run("mh_msg_stub", {3: hdr["MH_MSG_ROW0"] + i}, {(front, 1): f})
            good = good and how == ("blr", None) and cpu.r[3] == text0 + f * set_size + i * text_len and \
                cpu.written <= msg_ok
        ok(good, "msg stub, 0xF001 + i for each of the %d MAIN rows: row i of text set %d, the one on show "
           "(mh_text(i))" % (hdr["MH_MAIN_ROWS"], f))
    names = rac2_planet_names()
    good = len(names) == hdr["MH_PLANETS"]
    table = screens + hdr["MS_PICK_NAMES"]
    tmem = dict(((table + 4 * p, 4), struct.unpack(">I", data[table + 4 * p - hdr["MH_DATA"]:table + 4 * p + 4 - hdr["MH_DATA"]])[0])
                for p in range(hdr["MH_PLANETS"]))
    for p, name in enumerate(names):
        cpu, how = run("mh_msg_stub", {3: hdr["MH_MSG_PLANET"] + p, 4: 0x1234}, tmem)
        good = good and how == ("blr", None) and cstr(cpu.r[3]) == name and cpu.written <= msg_ok
    ok(good, "msg stub, 0x%X + p for each of the %d planets: PLANET_LIST's name of planet p (src/games/rac2.c), "
       "in the data image" % (hdr["MH_MSG_PLANET"], len(names)))
    # The pickers' round 2: the planet list's draw sign-extends a row's ids, so ours come as 0xFFFFF0xx too.
    good = True
    for p, name in enumerate(names):
        cpu, how = run("mh_msg_stub", {3: 0xFFFF0000 | (hdr["MH_MSG_PLANET"] + p), 4: 0x1234}, tmem)
        good = good and how == ("blr", None) and cstr(cpu.r[3]) == name and cpu.written <= msg_ok
    cpu, how = run("mh_msg_stub", {3: 0xFFFF0000 | hdr["MH_MSG_HELP"], 4: 0x1234})
    good = good and how == ("blr", None) and cstr(cpu.r[3]) == hdr["MH_ENTRY_LABEL"]
    for i in range(hdr["MH_MAIN_ROWS"]):
        cpu, how = run("mh_msg_stub", {3: 0xFFFF0000 | (hdr["MH_MSG_ROW0"] + i)}, {(front, 1): 1})
        good = good and how == ("blr", None) and cpu.r[3] == text0 + set_size + i * text_len
    ok(good, "msg stub, the same ids sign-extended (0xFFFFF0xx, as the planet list's draw passes them): the same "
       "answers, every planet's name among them, where round 1 went on into msg_string and its \"(unavailable)\"")
    for idn in (hdr["MH_MSG_ROW0"] + hdr["MH_MAIN_ROWS"], 0xF00F, hdr["MH_MSG_PLANET"] + hdr["MH_PLANETS"],
                0xF03F, 0xF040, 0xEFFF, 0xF0F0, 0x2D55, 0x2D4E, 0xFFFFFFFF,
                0xFFFF0000 | (hdr["MH_MSG_PLANET"] + hdr["MH_PLANETS"]), 0xFFFFF040, 0xFFFFEFFF, 0xFFFFF0F0,
                0xFFFF0000 | 0x2D55, 0x0001F010, 0x7FFFF010):
        cpu, how = run("mh_msg_stub", {3: idn, 4: 0x1234})
        ok(how == ("b", hdr["GAME_MSG_EXIT"]) and cpu.r[3] == idn and cpu.r[4] == 0x01340000 and
           cpu.written <= msg_ok,
           "msg stub, 0x%X: on into msg_string with r4 = 0x1340000 (its first instruction), r3 kept" % idn)

    # The inject, with no request: the game's own test, a leaf.
    inject_ok = {"r0", "r4", "r11", "r12", "cr0"}
    for edges, want in ((0x40, 0), (0x840, 0x800)):
        cpu, how = run("mh_inject_stub", {3: 0x333, 4: edges, 29: 0x29},
                       {(request, 1): 0, (pressed, 1): 1})
        ok(how == ("blr", None) and cpu.r[4] == want and cpu.r[3] == 0x333 and cpu.ld(pressed, 1) == 0 and
           cpu.written <= inject_ok,
           "inject, no request, presses 0x%X: r4 = 0x%X (the game's test), r3 kept, PRESSED down" % (edges, want))

    # The Start path tells the root stub.
    for p in (0, 1):
        cpu, how = run("mh_combo_stub", {3: 3, 4: 1, 6: 0x66}, {(pressed, 1): p, (force, 1): 1 - p})
        ok(how == ("blr", None) and cpu.r[5] == 1 and cpu.ld(force, 1) == p and cpu.r[3] == 3 and
           cpu.r[4] == 1 and cpu.r[6] == 0x66 and cpu.written <= {"r5", "r11", "r12"},
           "combo stub, PRESSED %d: r5 = 1, FORCE %d" % (p, p))

    # The root stub: the game's root, or the combo's screen over the ordinary root's bars.
    ui = 0x44000000
    pos = ui + hdr["R2_BARS_OFF"] + hdr["R2_R_POS"]
    combo = screens + hdr["MS_COMBO"] * hdr["R2_SCREEN_SIZE"]
    root_ok = {"r3", "r11", "r12", "cr0"}
    for f, mgr, table, want, what in (
            (0, ui, hdr["R2_POS_NORMAL"], hdr["R2_ROOT_SCREEN"], "FORCE down: the game's root"),
            (1, ui, hdr["R2_POS_NORMAL"], combo, "FORCE up over the ordinary root: the combo's screen, counted"),
            (1, ui, 0x01352174, hdr["R2_ROOT_SCREEN"], "FORCE up over a hoverbike root's bars: the game's root"),
            (1, ui, 0x01352258, hdr["R2_ROOT_SCREEN"], "FORCE up over an orbit root's bars: the game's root"),
            (1, ui, 0x0135218D, hdr["R2_ROOT_SCREEN"], "FORCE up, a table one byte off: the game's root"),
            (1, ui, 0x0136218C, hdr["R2_ROOT_SCREEN"], "FORCE up, a table 64 KB off: the game's root"),
            (1, 0, hdr["R2_POS_NORMAL"], hdr["R2_ROOT_SCREEN"], "FORCE up, no UI manager: the game's root")):
        mem = {(force, 1): f, (hdr["R2_UI_MANAGER"], 4): mgr, (pos, 4): table, (opens, 4): 7}
        cpu, how = run("mh_root_stub", {3: 0x01340000, 31: 0x015B2F50}, mem)
        counted = 8 if want == combo else 7
        ok(how == ("blr", None) and cpu.r[3] == want and cpu.ld(force, 1) == 0 and
           cpu.ld(opens, 4) == counted and cpu.r[31] == 0x015B2F50 and cpu.written <= root_ok,
           "root stub, %s (r3 = 0x%08X)" % (what, cpu.r[3]))


def rac2_planet_names():
    """PLANET_LIST's names for RaC2, out of src/games/rac2.c (rac2_planets)."""
    text = open(os.path.join(QWARK, "src", "games", "rac2.c"), encoding="utf-8").read()
    m = re.search(r"static const char \* const rac2_planets\[\] = \{(.*?)\};", text, re.S)
    return re.findall(r'"([^"]*)"', m.group(1)) if m else []


# The picker's rows that carry the planet's name alone: the location says the
# same (the Museum's, the Jamming Array's) or there is none (Wupash's, -1).
R2_PICK_NAME_ONLY = {21: 0x0B40, 25: 0xFFFFFFFF, 26: 0x271A}


def check_rac2_picker(ok, elf, hdr, data, dat, code, cave):
    """The planet picker (round 4): a copy of the planet select's screen 15 in
    the data image, with the game's galaxy and picture items, the shim's copy of
    its list over every planet qwark loads, and the game code it relies on."""
    W = elf.word
    scr = hdr["MH_SCREENS"]

    def dw(a):
        return struct.unpack(">I", data[a - dat:a - dat + 4])[0]

    def dh(a):
        return struct.unpack(">H", data[a - dat:a - dat + 2])[0]

    def ecstr(a):
        s = bytearray()
        while len(s) < 96:
            w = W((a + len(s)) & ~3)
            c = 0 if w is None else (w >> (8 * (3 - ((a + len(s)) & 3)))) & 0xFF
            if c == 0:
                break
            s.append(c)
        return s.decode("latin1")

    def desc_ok(d):
        if not dat <= d <= dat + len(data) - 8:
            return False
        c, t = dw(d), dw(d + 4)
        return cave <= c < cave + len(code) and dat + 0x8000 <= t <= dat + len(data) + 0x8000

    game, gal, lst, pic = hdr["R2_PICK_SCREEN"], hdr["R2_PICK_GALAXY"], hdr["R2_PICK_LIST"], hdr["R2_PICK_PICTURE"]
    pick, item, rows = scr + hdr["MS_PICK"], scr + hdr["MS_PICK_ITEM"], scr + hdr["MS_PICK_ROWS"]
    items, focus, parent = hdr["R2_SCR_ITEMS"], hdr["R2_SCR_FOCUS"], hdr["R2_SCR_PARENT"]
    n = hdr["MH_PLANETS"]

    # The game's planet select, screen 15, as the copy has it.
    gitems = [W(game + items + 4 * i) for i in range(14)]
    ok(W(0x00BB88A8) == 0x3063DD90 and W(0x00BB889C) == 0x3C600135 and W(game + 0xE4) == hdr["R2_PICK_ID"] and
       W(game + parent) == 0 and W(game + focus) == lst and
       gitems == [gal, lst, 0, 0, pic] + [0] * 9,
       "the planet select is screen 15 at 0x%08X (state 1, reason 4: 0xBB88A8), no parent, its focus the list, its "
       "items the galaxy 0x%08X, the list 0x%08X and the picture 0x%08X" % (game, gal, lst, pic))
    mine = [dw(pick + 4 * i) for i in range(hdr["R2_SCREEN_SIZE"] // 4)]
    ok(mine[:56] == [W(game + 4 * i) for i in range(56)] and mine[56] == 0 and mine[57] == hdr["R2_PICK_ID"] and
       mine[58] == item and mine[59:73] == [gal, item, 0, 0, pic] + [0] * 9 and mine[73:] == [0, 0, 0],
       "the picker's screen at 0x%08X: screen 15's rects and id, its parent 0 until the shim opens it, its focus "
       "and item 1 the shim's list, items 0 and 4 the game's galaxy and picture" % pick)
    ok(W(gal) == 0x012FD488 and W(0x012FD488) == 0x00BC9620 and W(0x00BC9638) == 0x4BF8AF45 and
       W(gal + 8) == 0x012FD498 and W(0x012FD498) == 0x00BC9690 and W(0x00BC96A0) == 0x90850234,
       "the galaxy item: its update hands the pad to the HD page (0xB5457C), its enter only clears 0x157D114")
    ok(W(pic) == 0x012FD620 and W(pic + 0x30) == 7 and W(0x01345870 + 4 * 7) == 0x012288F4 and
       ecstr(0x012288F4) == "rc2/ps3data/global/hud/gmap_ss/gmap_ss",
       "the picture item: the planet shown's gmap_ss[p] (every planet 0 .. 26 has one on the disc)")

    # The list: the game's item and the shim's copy.
    ok(W(lst) == hdr["OPD_LIST_UPDATE"] and W(hdr["OPD_LIST_UPDATE"]) == 0x00BBE528 and
       W(lst + 4) == hdr["R2_LIST_DRAW"] and W(hdr["R2_LIST_DRAW"]) == 0x00BCBC58 and
       W(lst + 8) == 0x012FD6A0 and W(0x012FD6A0) == 0x00BD2488 and W(lst + 0xC) == 0 and
       W(lst + hdr["R2_LI_FLAGS"]) == hdr["R2_LIST_FLAGS"] and W(lst + hdr["R2_LI_ROWS"]) == 0x015D167C and
       all(W(lst + o) == 0 for o in list(range(0x10, 0x30, 4)) + list(range(0x38, hdr["R2_LIST_SIZE"], 4))) and
       W(lst + hdr["R2_LIST_SIZE"]) == 0x012FD0F0,
       "the select's list 0x%08X: update 0xBBE528, draw 0xBCBC58, enter 0xBD2488, flags 0x538, rows 0x15D167C, "
       "the next item 0x%X on" % (lst, hdr["R2_LIST_SIZE"]))
    words = [dw(item + 4 * i) for i in range(hdr["R2_LIST_SIZE"] // 4)]
    ok(desc_ok(words[0]) and words[1] == hdr["R2_LIST_DRAW"] and desc_ok(words[2]) and desc_ok(words[3]) and
       len(set(words[:4])) == 4 and all(w == 0 for w in words[4:12]) and words[12] == hdr["R2_LIST_FLAGS"] and
       words[13] == rows and all(w == 0 for w in words[14:]),
       "the picker's list at 0x%08X: the shim's update, the game's draw, the shim's enter and exit, the select's "
       "flags, the shim's rows, the rest zero as the game's" % item)
    # The pickers' round 2: why the msg stub takes our ids sign-extended. The draw reads a row's text (+0) and its
    # second line (+8) as halfwords and sign-extends both before msg_string, whose answer for an id it does not
    # know is "(unavailable)" (0x133EE68): our 0xF0xx came to it as 0xFFFFF0xx and was not ours.
    ok(W(0x00BCC124) == 0x7C63FA2E and W(0x00BCC128) == 0x7C630734 and
       branch_target(0x00BCC12C, W(0x00BCC12C)) == 0x00B28F38 and
       W(0x00BCC384) == 0xA3BD0008 and W(0x00BCC390) == 0x7FA30734 and
       branch_target(0x00BCC398, W(0x00BCC398)) == 0x00B28F38 and
       W(0x00B28FD0) == 0x3C600134 and W(0x00B28FD4) == 0x3063EE68 and
       ecstr(0x0133EE68) == "(unavailable)",
       "the list's draw (0xBCBC58) sign-extends a row's two text ids (lhzx/extsh at 0xBCC124, lhz/extsh at "
       "0xBCC384) before msg_string, which answers an id it does not know with \"(unavailable)\": the msg stub "
       "takes 0xFFFFF0xx as 0xF0xx")

    # The rows: every planet qwark loads, in its order.
    good = True
    for p in range(n):
        r = rows + p * hdr["R2_ROW_SIZE"]
        text, kind, target, sub, anim = dh(r), dh(r + 2), dw(r + 4), dh(r + 8), dh(r + 10)
        loc = W(hdr["R2_PLANET_TEXT"] + 8 * p + 4)
        name = hdr["MH_MSG_PLANET"] + p
        if p in R2_PICK_NAME_ONLY:
            good = good and loc == R2_PICK_NAME_ONLY[p] and text == name and sub == 0
        else:
            good = good and 0 < loc < 0x10000 and text == loc and sub == name
        good = good and kind == hdr["R2_ROW_INERT"] and target == 0 and anim == 0
    end = rows + n * hdr["R2_ROW_SIZE"]
    ok(good and all(b == 0 for b in data[end - dat:end - dat + hdr["R2_ROW_SIZE"]]) and n == len(rac2_planet_names()),
       "the picker's %d rows: planet p's location (the game's text id at 0x%08X + 8p + 4) over its name (0x%X + p), "
       "or its name alone for the Museum, Wupash and the Jamming Array; kind 0x%X, no target; then a zero row" %
       (n, hdr["R2_PLANET_TEXT"], hdr["MH_MSG_PLANET"], hdr["R2_ROW_INERT"]))

    # The game code the picker relies on.
    ctx = [
        (0x00BB77FC, 0x2C830007, "the row actions: cmpwi cr1,r3,7 ..."),
        (0x00BB7840, 0x2C03000D, "... cmpwi r3,13, the highest kind acted on ..."),
        (0x00BB7870, 0x48000350, "... and any other kind does nothing (b 0xBB7BC0): 0x101 is inert"),
        (0x00BBE6C8, 0x39200900, "the list's update: Start or Select ..."),
        (0x00BBE704, 0x3860FFFF, "... close the pause (-1)"),
        (0x00BBE70C, 0x550906F6, "Triangle ..."),
        (0x00BBE740, 0x806300E0, "... reads the screen's parent ..."),
        (0x00BBE74C, 0x907E0018, "... and makes it the manager's next screen"),
        (0x00BBE770, 0x3C600142, "the cross: the confirm mask 0x141ABA8 ..."),
        (0x00BBE774, 0x8063ABA8, ""),
        (0x00BBE784, 0x3C600157, "... unless bit 0 of 0x156A00C ..."),
        (0x00BBE788, 0x8063A00C, ""),
        (0x00BBE78C, 0x546307FE, ""),
        (0x00BBE80C, 0x4BFF8FE1, "... runs the row's action (bl 0xBB77EC)"),
        (0x00BBE9C0, 0x80635AB4, "a cursor moved shows the planet of the game's discovered list (0x1345AB4) ..."),
        (0x00BBE9D0, 0x4BF8F529, "... through 0xB4DEF8"),
        (0x00B4DF08, 0x3FE00158, "0xB4DEF8 writes the planet shown, 0x157D110"),
        (0x00B4DF10, 0x33FFCEE0, ""),
        (0x00B4DF1C, 0x907F0230, ""),
        (0x00BB91FC, 0x38800004, "an update's close is exit action 2 only for pause reason 4 ..."),
        (0x00BB9228, 0x907F001C, "... (1 for the Trainer's reason 1)"),
        (0x00BB95BC, 0x2C030002, "action 2 would pop with the planet (mode_pop(0, *0x157D110)) ..."),
        (0x00BB9694, 0x38600000, "... and action 1 pops with none: mode_pop(0, 0)"),
        (0x00BB9698, 0x38800000, ""),
        (0x00BB969C, 0x4BFD2AD5, ""),
        (0x00BC983C, 0x4BFC270D, "the planet page's cross (screen 16) requests the game's travel, mode 6: never reached"),
        (0x00BCAA2C, 0x30A5E264, "and its update tells screen 16 by its address, so it cannot be copied"),
    ]
    for a, want, what in ctx:
        ok(W(a) == want, "0x%08X holds 0x%08X%s" % (a, want, (": " + what) if what else ""))
    ok(branch_target(0x00BBE80C, W(0x00BBE80C)) == 0x00BB77EC and branch_target(0x00BBE9D0, W(0x00BBE9D0)) == 0x00B4DEF8
       and branch_target(0x00BB969C, W(0x00BB969C)) == 0x00B8C170 and branch_target(0x00BC983C, W(0x00BC983C)) == 0x00B8BF48,
       "the calls: the row actions 0xBB77EC, the planet shown 0xB4DEF8, mode_pop 0xB8C170, mode_requestChange 0xB8BF48")


def check_rac2(ok, elf, roots):
    hdr = header_defs(os.path.join(HERE, "mh_rac2.h"))
    box_defs = header_defs(MENUBOX)
    shim_defs = header_defs(SHIM_H)
    box, data, code, hooks, caves, core = parse_bins(GEN_C, "rac2")
    print("\nNPEA00386 console menu renderer")
    if box is None or data is None or code is None or core is None:
        ok(False, "menuhelper_bins.c has rac2's mailbox, data, code and the core's place in it")
        return

    cave, cave_end = hdr["MH_CAVE"], hdr["MH_CAVE_END"]
    mbx, mbx_end = hdr["MH_BOX"], hdr["MH_BOX_END"]
    dat, dat_end = hdr["MH_DATA"], hdr["MH_DATA_END"]
    tv, tfs, tms, _, _ = elf.text
    rv, rfs, rms, _, _ = elf.rw
    W = elf.word

    ok(len(caves) == 4 and [c[:3] for c in caves] ==
       [(mbx, "mh_rac2_box", 0), (dat, "mh_rac2_state", 0), (caves[2][0], "mh_rac2_data", 0),
        (cave, "mh_rac2_code", 1)] and caves[2][3] == 0 and caves[3][3] == 0,
       "four caves: the mailbox at 0x%08X, the state at 0x%08X, the screens and read-only data, then the code at "
       "0x%08X" % (mbx, dat, cave))
    rest = parse_rest(GEN_C, "rac2")
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
    ok(box[8] == hdr["MH_ROWS"] == 32 and box[9] == hdr["MH_ROW_BYTES"] == box_defs["MBOX_ROW_SIZE"] and
       box[10] == hdr["MH_MAIN_ROWS"] == 7 and box[11] == hdr["MH_CAPS"] and box[12] == 0 and box[13] == 0 and
       box[14] == 2 and box[15] == hdr["MH_CFG_ROWS"] == 32 and (box[11] & box_defs["MBOX_CAP_PICKER"]) != 0,
       "row_cap 32 (round 4), rows of 0x40, 7 rows a MAIN page, 32 a CONFIG page, caps 0x%02X (the planet "
       "picker's among them), closed, no request, renderer 2" % box[11])
    ok(box_defs["MBOX_OFF_ROWS"] + hdr["MH_ROWS"] * hdr["MH_ROW_BYTES"] == hdr["MH_MBOX_SIZE"] and
       all(b == 0 for b in box[16:]),
       "the rows fill the image, everything after the header zero (the pick words at 0x%X and 0x%X among it)" %
       (box_defs["MBOX_OFF_PICK_SEQ"], box_defs["MBOX_OFF_PICK_PLANET"]))
    shim_off = hdr["MH_SHIM"] - hdr["MH_STATE"]
    ok(struct.unpack(">I", data[0:4])[0] == hdr["MH_MAGIC"] and
       all(b == 0 for b in data[4:shim_off + hdr["SH_SIZE"]]),
       "the data image: the core's state at MH_STATE with its magic 'QMH2', the shim's at MH_SHIM "
       "(+0x%X), everything else zero" % shim_off)

    # 7a. the screens, the first of the read-only data
    scr = hdr["MH_SCREENS"]
    so = scr - dat
    ok(scr == hdr["MH_SHIM"] + hdr["SH_SIZE"] == caves[2][0] and scr % 16 == 0 and so + hdr["MS_SIZE"] <= len(data) and
       hdr["MH_SHIM"] - hdr["MH_STATE"] == (shim_defs["ST_SET0"] + 2 * shim_set_layout(shim_defs, hdr["MH_ROWS"])[1] + 7) & ~7,
       "the screens at 0x%08X, right after the shim's state (after the core's, with its two text sets): the first "
       "bytes of the read-only data" % scr)

    def dw(a):
        return struct.unpack(">I", data[a - dat:a - dat + 4])[0]

    item = scr + hdr["MS_ITEM"]
    size = hdr["R2_SCREEN_SIZE"]
    root_rects = [W(hdr["R2_ROOT_SCREEN"] + 4 * i) for i in range(56)]
    cheat_rects = [W(hdr["R2_CHEATS_SCREEN"] + 4 * i) for i in range(56)]
    ok(W(hdr["R2_ROOT_SCREEN"] + 0xE4) == 2 and W(hdr["R2_ROOT_SCREEN"] + 0xE8) == 0x013468A4 and
       W(hdr["R2_CHEATS_SCREEN"] + 0xE4) == 9 and W(hdr["R2_CHEATS_SCREEN"] + 0xE8) == 0x0134A404,
       "the root (0x13460E4) is id 2 with its item 0x13468A4, Cheats (0x134A2D4) id 9 with its item 0x134A404")
    for k, name, rects, parent, idn in ((hdr["MS_HELP"], "the Help entry's", root_rects, hdr["R2_ROOT_SCREEN"], 2),
                                        (hdr["MS_COMBO"], "the combo's", root_rects, 0, 2),
                                        (hdr["MS_MAIN_A"], "MAIN A", root_rects, 0, 2),
                                        (hdr["MS_MAIN_B"], "MAIN B", root_rects, 0, 2),
                                        (hdr["MS_CFG_A"], "CONFIG A", cheat_rects, 0, 9),
                                        (hdr["MS_CFG_B"], "CONFIG B", cheat_rects, 0, 9)):
        s = scr + k * size
        mine = [dw(s + 4 * i) for i in range(size // 4)]
        ok(mine[:56] == rects and mine[56] == parent and mine[57] == idn and mine[58] == item and
           mine[59] == item and all(w == 0 for w in mine[60:]),
           "screen %d, %s: the %s rects, parent 0x%08X, id %d, its focus and only item the shim's" %
           (k, name, "root's" if idn == 2 else "Cheats'", parent, idn))
    words = [dw(item + 4 * i) for i in range(hdr["R2_ITEM_SIZE"] // 4)]
    descs_ok = True
    for d in words[:4]:
        if not dat <= d <= dat + len(data) - 8:
            descs_ok = False
            continue
        c, t = struct.unpack(">II", data[d - dat:d - dat + 8])
        descs_ok = descs_ok and cave <= c < cave + len(code) and dat + 0x8000 <= t <= dat + len(data) + 0x8000
    ok(descs_ok and len(set(words[:4])) == 4 and words[4] == 1 and all(w == 0 for w in words[5:]) and
       W(0x013468B4) == 1 and W(0x0134A414) == 1,
       "the item: update, draw, enter and exit, four descriptors {code in the image, the renderer's TOC}, then "
       "the 1 the root's and Cheats' items have at +0x10")
    ids = [dw(scr + hdr["MS_IDS"] + 4 * i) for i in range(hdr["MH_MAIN_ROWS"] + 1)]
    ok(ids == [hdr["MH_MSG_ROW0"] + i for i in range(hdr["MH_MAIN_ROWS"])] + [0],
       "the MAIN rows' text ids, 0xF001 .. 0x%X" % (hdr["MH_MSG_ROW0"] + hdr["MH_MAIN_ROWS"] - 1))
    strs = data[so + hdr["MS_TRAINER"]:so + hdr["MS_MORE"] + 4]
    ok(strs == hdr["MH_ENTRY_LABEL"].encode() + b"\0...\0",
       "\"%s\" and \"...\" after them" % hdr["MH_ENTRY_LABEL"])

    check_rac2_picker(ok, elf, hdr, data, dat, code, cave)

    # 1. the originals, 2. the hook words
    order = ["MH_SITE_MSG", "MH_SITE_INJECT", "MH_SITE_COMBO", "MH_SITE_ROOT", "MH_SITE_HELP_SCREEN",
             "MH_SITE_HELP_LABEL"]
    ok(len(hooks) == 6 and [h[0] for h in hooks] == [hdr[k] for k in order],
       "six words: the label hook, the Start path's three, then the root's Help entry's two")
    for a, value, orig, what in hooks:
        ok(W(a) == orig, "0x%08X holds 0x%08X in the executable (%s)" % (a, orig, what))

    stub_at = {}
    for a, value, orig, what in hooks:
        m = re.match(r"(b|bl) (mh_\w+_stub)", what)
        if m:
            link = m.group(1) == "bl"
            t = branch_target(a, value) if (value >> 26) == 18 and (value & 3) == (1 if link else 0) else None
            first = struct.unpack(">I", code[t - cave:t - cave + 4])[0] \
                if t is not None and cave <= t < cave + len(code) else None
            mask, want, name = STUB_FIRST_RAC2[m.group(2)]
            good = first is not None and (first & mask) == want
            ok(good, "0x%08X: %08X = %s 0x%08X, %s (%s)" % (a, value, m.group(1), t or 0, m.group(2), name))
            if good:
                stub_at[m.group(2)] = t
        elif a == hdr["MH_SITE_HELP_SCREEN"]:
            ok(value == scr + hdr["MS_HELP"] * size,
               "0x%08X: the Help entry's screen 0x%08X becomes the menu's Help screen 0x%08X" % (a, orig, value))
        elif a == hdr["MH_SITE_HELP_LABEL"]:
            ok(value == hdr["MH_MSG_HELP"], "0x%08X: the Help label 0x%X becomes 0x%X, \"Trainer\"" % (a, orig, value))

    # The sites in the game's code, and around them.
    ctx = [
        (0x012FA7F8, 0x00B28F38, "msg_string's descriptor names 0xB28F38, the label hook's site"),
        (0x00B28F3C, 0x38E00000, "li r7,0 after it, where the stub goes on"),
        (0x00BF60EC, 0x4BF95D7D, "the mode 0 update calls 0xB8BE68 before the Start test (volatiles dead)"),
        (0x00BF60F8, 0x807E2294, "r3 = the player's state, which the gate reads after the test"),
        (0x00BF6104, 0x3C800148, "the pad 0x147A380 ..."),
        (0x00BF6108, 0x3084A380, "..."),
        (0x00BF610C, 0x808400A4, "... and its presses, +0xA4, before the inject site"),
        (0x00BF6114, 0x2C040000, "cmpwi r4,0 after it"),
        (0x00BF6118, 0x40820014, "bne 0xBF612C, the Start path"),
        (0x00BF6130, 0x8084B06C, "the gate: the frame count 0x156B06C ..."),
        (0x00BF613C, 0x2C03006F, "... and r3, the player's state"),
        (0x00BF6188, 0x38600003, "the Start path: li r3,3"),
        (0x00BF618C, 0x38800001, "li r4,1"),
        (0x00BF6194, 0x38C00000, "li r6,0 after the combo site"),
        (0x00BF6198, 0x38E00000, "li r7,0"),
        (0x00BF619C, 0x4BF95DAD, "bl 0xB8BF48, mode_requestChange(3, 1, 1, 0, 0)"),
        (0x00BB8804, 0x93DF00E0, "the manager's state 1 puts 0 in its previous screen"),
        (0x00BB8954, 0x3C600134, "reason 1: lis r3,0x134 before the root site"),
        (0x00BB895C, 0x907F0014, "after it: stw r3, the current screen"),
        (0x00BB8960, 0x907F0018, "stw r3, the next"),
        (0x00BB903C, 0x80830008, "state 4 calls each item's enter on its first frame"),
        (0x00BB92D4, 0x7C851800, "a next screen: cmpw cr1, the current one ..."),
        (0x00BB92F8, 0x38600012, "... sound 18 for another screen"),
        (0x00BB932C, 0x8083000C, "then each item's exit"),
        (0x00BB937C, 0x90DF00E0, "and the old screen kept as the previous"),
        (0x00BB97A4, 0x2C03000A, "a close waits for the pause's tenth frame ..."),
        (0x00BB97FC, 0x8083000C, "... and calls each item's exit"),
        (0x013467F4, 0x2D550003, "the root's entry 4: text 11605, kind 3 (open the screen at +4)"),
        (0x012FD150, 0x00BBEE54, "the root item's draw descriptor"),
        (0x013468A8, 0x012FD150, "the root's item draws with it"),
        (0x00BBEE6C, 0x4BF953C9, "which draws the bars (0xB54234) between Cheats' begin and end"),
        (0x00BC1A44, 0x43500000, "Cheats' draw: the frame at y 208.0 ..."),
        (0x00BC1A48, 0x3F800000, "... 1.0 ..."),
        (0x00BC1A4C, 0x3F570A3D, "... 0.84"),
        (0x00BC1AC4, 0x3880008B, "sprite 139 ..."),
        (0x00BC1AE8, 0x3CC06044, "... in 0x60442D00"),
        (0x00BC1AEC, 0x60C62D00, ""),
        (0x00BC1B10, 0xC09B5698, "its scale from 0x1A25698"),
        (0x00BC1B50, 0x3CC055F0, "sprites 140, 141 in 0x55F0C070"),
        (0x00BC1B60, 0x60C6C070, ""),
        (0x00BC1C14, 0x38800041, "the title at y 65"),
        (0x00BC1C30, 0x38602D4E, "hint 11598 ..."),
        (0x00BC1C4C, 0x38800141, "... at y 321"),
        (0x00BC1C68, 0x38602D4F, "hint 11599 ..."),
        (0x00BC1C88, 0x3880015A, "... at y 346"),
        (0x00BC1CBC, 0x3AA00070, "the rows from y 112"),
        (0x00BC1D04, 0x3C807029, "the cursor's row in 0x7029A1FF"),
        (0x00BC1D0C, 0x6084A1FF, ""),
        (0x00BC1D44, 0x3860002B, "the label at x 43"),
        (0x00BC1DBC, 0x38A00195, "the value centred on x 405"),
    ]
    for a, want, what in ctx:
        ok(W(a) == want, "0x%08X holds 0x%08X%s" % (a, want, (": " + what) if what else ""))
    ok(branch_target(hdr["MH_SITE_COMBO"] + 12, W(hdr["MH_SITE_COMBO"] + 12)) == 0x00B8BF48,
       "the combo site's call is mode_requestChange 0xB8BF48")

    # 4a. the root's Help entry and its labels: Help's two words and nothing else
    lab_at, labels = R2_ROOT_LABELS
    ok([W(lab_at + 4 * i) for i in range(7)] == list(labels) and lab_at + 16 == hdr["MH_SITE_HELP_LABEL"],
       "the root's seven labels at 0x%08X, Help's the fifth (0x%08X)" % (lab_at, hdr["MH_SITE_HELP_LABEL"]))
    others = [W(t + 4 * i) for t in R2_OTHER_LABELS for i in range(5)]
    ok(hdr["MH_ORIG_HELP_LABEL"] not in others,
       "the label tables of the hoverbike, ship and orbit roots after it have no Help: the menu has no way in there")
    setup = [a for a, v in refs_into(elf, 0x0135211C, 0x01352120)[2] if 0x00BB6970 <= a < 0x00BB70E0] + \
            [a for a, v in refs_into(elf, 0x0135218C, 0x01352190)[2] if 0x00BB6970 <= a < 0x00BB70E0]
    ok(len(setup) >= 2, "the root's setup (0xBB6970) names the seven labels and the table 0x%08X the root stub "
       "tests" % hdr["R2_POS_NORMAL"])
    # The entry is a row {text, kind, screen}: what names it reads the screen in the word.
    row = hdr["MH_SITE_HELP_SCREEN"] - 4
    br, wd, pr = refs_into(elf, row, row + 0x18)
    ok(len(pr) == 1 and 0x00BBA194 <= pr[0][0] < 0x00BBA300 and [a for a, _ in wd] == [0x013469F8] and
       W(0x013469C4) == 0x012FD138 and W(0x012FD138) == 0x00BBE528 and
       not refs_into(elf, 0x013469C4, 0x013469C8)[2],
       "Help's row (0x%08X) is read by the root's confirm of bar 4 (0xBBA194) and named by the PS2 list's item "
       "for it (0x13469C4, rows +0x34), which no code names: either way the row opens the screen in the word" % row)
    br, wd, pr = refs_into(elf, hdr["MH_SITE_HELP_LABEL"], hdr["MH_SITE_HELP_LABEL"] + 4)
    ok(not wd and not pr, "nothing names the Help label's word but its table")

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
    inject = stub_at.get("mh_inject_stub")
    if inject is not None:
        iw = [struct.unpack(">I", code[inject - cave + 4 * i:inject - cave + 4 * i + 4])[0] for i in range(25)]
        t = branch_target(inject + 4 * 16, iw[16])
        ok(iw[7] == 0x4D820020 and iw[8] == 0xF821FF81 and iw[9] == 0x7C0802A6 and iw[10] == 0xF8010090 and
           iw[11] == 0xF8410078 and iw[12] == 0xF8610070 and iw[13] == 0x60830000 and
           (iw[16] >> 26) == 18 and (iw[16] & 3) == 1 and t is not None and cave <= t < cave + len(code) and
           iw[17] == 0x60000000 and iw[18] == 0x60640000 and iw[19] == 0xE8610070 and iw[20] == 0xE8410078 and
           iw[21] == 0xE8010090 and iw[22] == 0x7C0803A6 and iw[23] == 0x38210080 and iw[24] == 0x4E800020,
           "the inject stub, with a request: a frame of 0x80, LR, r2 and r3 kept, mh_inject(r4) with the "
           "renderer's TOC, its answer in r4, all three back")

    opds = dict((v, k) for k, v in hdr.items() if k.startswith("OPD_") and not k.endswith("#comment"))
    bad, seen = calls_via_descriptors(code, cave, set(opds), elf, toc=RAC2_TOC, code_hi=0x12D6668)
    ok(not bad, "every indirect call switches r2 through one of the header's descriptors and back")
    for b in bad[:12]:
        print("        " + b)
    ok(seen == set(opds), "and every descriptor the header names is one the code calls (%d of %d)" %
       (len(seen), len(opds)))
    for name in sorted(opds.values()):
        a = hdr[name]
        m = re.search(r"(0x[0-9A-Fa-f]+)\s*$", hdr.get(name + "#comment", ""))
        fn = int(m.group(1), 16) if m else None
        ok(fn is not None and W(a) == fn and W(a + 4) == RAC2_TOC,
           "%s 0x%08X = {0x%08X, TOC 0x%08X}" % (name, a, fn or 0, RAC2_TOC))
    lv = [hdr[k] for k in ("OPD_LV_RACE", "OPD_LV_SHIP", "OPD_LV_SHIP_ON", "OPD_LV_ORBIT", "OPD_LV_OTHER",
                           "OPD_LV_PSTATE")]
    lv_fn = [W(d) for d in lv]
    asked = [branch_target(a, W(a)) for a in range(0x00BB6970, 0x00BB70E0, 4)
             if (W(a) >> 26) == 18 and (W(a) & 3) == 1]
    ok(all(f in asked for f in lv_fn), "the root's setup (0xBB6970) asks all six questions mh_inject asks")

    # 8. the leaf stubs, stepped
    if all(k in stub_at for k in ("mh_msg_stub", "mh_inject_stub", "mh_combo_stub", "mh_root_stub")):
        try:
            check_rac2_stubs(ok, code, cave, hdr, box_defs, shim_defs, stub_at, data)
        except ValueError as e:
            ok(False, "the leaf stubs step: %s" % e)
    else:
        ok(False, "every stub found to step")

    # 9. overlaps
    ours = [(cave, cave + len(code), "renderer code"), (mbx, mbx + len(box), "renderer mailbox"),
            (dat, dat + len(data), "renderer data")]
    ours += [(a, a + 4, "renderer word") for a, _, _, _ in hooks]
    known = mod_entries(roots, "NPEA00386")
    clashes, accepted = [], []
    for lo, hi, what in ours:
        for a, b, label, mod, kind in known:
            if a < hi and b > lo:
                text = "%s 0x%08X..0x%08X vs %s/%s %s 0x%08X..0x%08X" % (what, lo, hi, label, mod, kind, a, b)
                (accepted if mod in ACCEPTED_RAC2 and what == "renderer word" else clashes).append((text, mod))
        for a, b, w in QWARK_RAC2:
            if a < hi and b > lo:
                clashes.append(("%s 0x%08X..0x%08X vs qwark %s 0x%08X..0x%08X" % (what, lo, hi, w, a, b), None))
    ok(not clashes, "no overlap with %d mod words and caves (the probes' among them), nor with qwark's %d own "
       "(the savefile helper's and the switches' caves, the aside buffer among them)" % (len(known), len(QWARK_RAC2)))
    for c, _ in clashes:
        print("        " + c)
    named = [x for x in mod_pointers(roots, "NPEA00386", cave, cave_end) +
             mod_pointers(roots, "NPEA00386", mbx, mbx_end) +
             mod_pointers(roots, "NPEA00386", dat, dat_end)]
    ok(not named, "no mod's code names an address in any of the three areas")
    for x in named[:12]:
        print("        " + x)
    seen_acc = set()
    for c, mod in accepted:
        if mod not in seen_acc:
            print("        known: %s/...: %s" % (mod, ACCEPTED_RAC2[mod]))
            seen_acc.add(mod)
    print("        (%d words shared with the prototypes by design)" % len(accepted))


# ------------------------------------------------------------ Ratchet & Clank

RAC1_TOC = 0x00722328
RAC1_OPD = (0x00703548, 0x0071A328)     # the executable's .opd

# qwark's own writes into RaC1, build 54: src/games/rac1*.{c,h},
# src/games/sfhelper/sf_rac1.h, src/games/switches/sw_rac1.ld. (lo, hi, what).
QWARK_RAC1 = [
    (0x004F63C0, 0x004F6600, "the savefile helper's old trampoline and syscall thunk (dead code qwark keeps clear)"),
    (0x004F6800, 0x004F6900, "the savefile helper's stub"),
    (0x004F69B8, 0x004F7120, "code switches (RPCS3)"),
    (0x004F8000, 0x004F8C68, "the savefile helper's cave"),
    (0x0011E3A0, 0x0011E3A4, "the savefile helper's hook"),
    (0x0007F558, 0x0007F560, "freeze health's site and the fingerprint"),
    (0x000AA2DC, 0x000AA2E0, "infinite ammo"),
    (0x00160060, 0x00160064, "fast loads"),
    (0x001641F8, 0x001641FC, "fast loads"),
    (0x00165490, 0x00165494, "fast loads"),
    (0x00FC0000, 0x01070000, "savefile aside buffer"),
    (0x01080000, 0x01080010, "savefile helper request bytes"),
]

# Overlaps that are by design: (mod folder) -> why.
ACCEPTED_RAC1 = {
    "probe-n1": "probe N1 hooks the same text and play words; the install refuses while it is loaded",
    "probe-n1b": "probe N1b is the Help row's prototype and hooks the same four words; the install refuses while it is loaded",
    "probe-n1c": "probe N1c is this renderer's prototype and hooks the same four words; the install refuses while it is loaded",
}

# The stubs' first words (mh_rac1_stubs.S), by the symbol in the hook's comment.
STUB_FIRST_RAC1 = {
    "mh_text_stub": (0xFFFFFFFF, 0x38A48200, "addi r5,r4,-0x7E00"),
    "mh_play_stub": (0xFFFFFFFF, 0xF821FEA1, "stdu r1,-0x160(r1)"),
}

# The runs of code nothing reaches the renderer's four code areas are, each
# with the descriptors that name its functions and nothing names in turn.
RAC1_DEAD = {
    "MH_CAVE": (0x00717290, 0x00717450),
    "MH_CAVE2": (0x00711110, 0x00711118),
    "MH_CAVE3": (0x00710B08, 0x00710B20),
    "MH_CAVE4": (0x00713490, 0x007134A8),
}


class Ranges:
    """A set of address ranges, for lint_code's `allowed`."""

    def __init__(self, ranges):
        self.ranges = ranges

    def __contains__(self, a):
        return any(lo <= a < hi for lo, hi in self.ranges)


def R1_ID_OK(hdr):
    return hdr["R1_ID_BASE"] < hdr["R1_ID_VELDIN2"] < hdr["R1_ID_BASE"] + hdr["R1_ID_COUNT"] and \
        hdr["R1_ID_VELDIN2"] not in (hdr["R1_ID_TRAINER"], hdr["R1_ID_TITLE"]) and \
        not hdr["R1_ID_ROW0"] <= hdr["R1_ID_VELDIN2"] < hdr["R1_ID_ROW0"] + hdr["MH_MAIN_ROWS"]


def check_rac1_stubs(ok, code, cave, hdr, stub_at, in_code):
    strtab, hits = hdr["MH_SHIM"] + hdr["SH_STRTAB"], hdr["MH_SHIM"] + hdr["SH_HITS"]
    text_ok = {"r3", "r5", "r6", "r7", "cr0"}

    def text(idn, entry=None):
        cpu = Ppc(code, cave, {})
        if entry is not None:
            cpu.st(strtab + 4 * ((idn - hdr["R1_ID_BASE"]) & 0xF), entry, 4)
        cpu.st(hits, 7, 4)
        cpu.r[3] = 0x333
        cpu.r[4] = idn
        cpu.r[8] = 0x888
        return cpu, cpu.run(stub_at["mh_text_stub"])

    good = True
    for i in range(hdr["R1_ID_COUNT"]):
        cpu, how = text(hdr["R1_ID_BASE"] + i, 0x0160C000 + 0x40 * i)
        good = good and how == ("blr", None) and cpu.r[3] == 0x0160C000 + 0x40 * i and \
            cpu.ld(hits, 4) == 8 and cpu.written <= text_ok and cpu.r[4] == hdr["R1_ID_BASE"] + i
    ok(good, "text stub, ids 0x7E00..0x7E0F with an entry: the shim's string, a hit counted, back to "
             "get_ui_string's caller")
    cpu, how = text(hdr["R1_ID_TRAINER"], 0)
    ok(how == ("blr", None) and cpu.r[3] == hdr["R1_MSG_MISSING"] and cpu.ld(hits, 4) == 7 and cpu.written <= text_ok,
       "text stub, an id of the shim's with no entry: \"MESSAGE DOES NOT EXIST\", as the game had it")
    for idn in (0x7DFF, hdr["R1_ID_BASE"] + hdr["R1_ID_COUNT"], 0x4EE6, 0x0, 0xFFFF):
        cpu, how = text(idn)
        ok(how == ("blr", None) and cpu.r[3] == hdr["R1_MSG_MISSING"] and cpu.ld(hits, 4) == 7 and
           cpu.r[8] == 0x888 and cpu.written <= text_ok,
           "text stub, 0x%X: \"MESSAGE DOES NOT EXIST\" (0x720B94), r8 kept" % idn)

    play = stub_at["mh_play_stub"]
    pw = [struct.unpack(">I", code[play - cave + 4 * i:play - cave + 4 * i + 4])[0] for i in range(96)]
    calls = [i for i, w in enumerate(pw) if (w >> 26) == 18 and (w & 3) == 1]
    ok(pw[0] == 0xF821FEA1 and pw[2] == 0x7C0802A6 and pw[3] == 0xF8010170 and pw[4] == 0xF8410078 and len(calls) == 1 and
       branch_target(play + 4 * calls[0], pw[calls[0]]) in in_code and
       pw[calls[0] + 1] == 0x60000000 and 0x4E800020 in pw and
       pw[pw.index(0x4E800020) - 1] == 0x38210160,
       "the play stub saves every volatile register, CR, XER and CTR, calls mh_play with the renderer's TOC, "
       "puts them all back and returns to the main loop")


def check_rac1(ok, elf, roots):
    hdr = header_defs(os.path.join(HERE, "mh_rac1.h"))
    box_defs = header_defs(MENUBOX)
    shim_defs = header_defs(SHIM_H)
    box, data, code, hooks, caves, core = parse_bins(GEN_C, "rac1")
    print("\nNPEA00385 console menu renderer")
    text = open(GEN_C, encoding="utf-8").read()

    def array(name):
        m = re.search(r"static const u8 %s\[\] = \{(.*?)\};" % name, text, re.S)
        return bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))) if m else None

    names = ["MH_CAVE", "MH_CAVE2", "MH_CAVE3", "MH_CAVE4"]
    codes = [code] + [array("mh_rac1_code%d" % n) for n in (2, 3, 4)]
    if box is None or data is None or None in codes or core is None:
        ok(False, "menuhelper_bins.c has rac1's mailbox, data, four code areas and the core's place in them")
        return

    cave, cave_end = hdr["MH_CAVE"], hdr["MH_CAVE_END"]
    mbx, mbx_end = hdr["MH_BOX"], hdr["MH_BOX_END"]
    dat, dat_end = hdr["MH_DATA"], hdr["MH_DATA_END"]
    tv, tfs, tms, _, _ = elf.text
    rv, rfs, rms, _, _ = elf.rw
    areas = tuple((hdr[nm], hdr[nm + "_END"], c, nm) for nm, c in zip(names, codes))
    in_code = Ranges([(lo, lo + len(c)) for lo, _, c, _ in areas])

    ok(len(caves) == 7 and [c[:3] for c in caves] ==
       [(mbx, "mh_rac1_box", 0), (dat, "mh_rac1_state", 0), (caves[2][0], "mh_rac1_data", 0)] +
       [(lo, "mh_rac1_code" + ("%d" % (k + 1) if k else ""), 1) for k, (lo, _, _, _) in enumerate(areas)] and
       all(c[3] == 0 for c in caves[2:]),
       "seven caves: the mailbox at 0x%08X, the state at 0x%08X, the read-only data, then the code at "
       "%s" % (mbx, dat, ", ".join("0x%08X" % a[0] for a in areas)))
    rest = parse_rest(GEN_C, "rac1")
    ok(caves[0][3] > 0 and caves[1][3] > 0 and caves[2][0] == dat + len(data) - len(rest),
       "the mailbox and the state are a few bytes and then zeros, %d and %d of them, carried as counts; "
       "the read-only data follows the state" % (caves[0][3], caves[1][3]))
    ok(all(lo + len(c) <= hi for lo, hi, c, _ in areas) and mbx + len(box) <= mbx_end and dat + len(data) <= dat_end,
       "code %s bytes (of %s), mailbox %d bytes (of %d), data %d bytes (of %d)" %
       ("+".join(str(len(c)) for _, _, c, _ in areas), "+".join(str(hi - lo) for lo, hi, _, _ in areas),
        len(box), mbx_end - mbx, len(data), dat_end - dat))

    # 0. the core: in the first area, after the stubs
    check_core(ok, hdr, code, cave, core)

    # 6. the areas: code nothing reaches, in the text segment; the data past the read-write segment's end
    starts = set()
    for a, w in elf.words(elf.text):
        if (w >> 26) == 18 and (w & 1) == 1:
            starts.add(branch_target(a, w))
    for a in range(RAC1_OPD[0], RAC1_OPD[1], 8):
        if elf.word(a + 4) == RAC1_TOC:
            starts.add(elf.word(a))
    for lo, hi, c, name in areas:
        dlo, dhi = RAC1_DEAD[name]
        ok(tv <= lo and hi <= tv + tfs and lo in starts and hi in starts,
           "%s 0x%08X..0x%08X is in the text segment, from a function's start to the next's" % (name, lo, hi))
        br, wd, pr = refs_into(elf, lo, hi)
        outside = [(a, t) for a, t in br if not lo <= a < hi]
        opd_words = [(a, w) for a, w in wd if RAC1_OPD[0] <= a < RAC1_OPD[1] and elf.word(a + 4) == RAC1_TOC]
        ok(not outside and not pr and sorted(opd_words) == sorted(wd) and
           all(dlo <= a < dhi for a, _ in opd_words),
           "nothing branches into it from outside, no lis/addi pair names it, and the only words that do are its "
           "own functions' descriptors 0x%08X..0x%08X (%d)" % (dlo, dhi, len(opd_words)))
        br, wd, pr = refs_into(elf, dlo, dhi)
        ok(not wd and not pr, "and nothing names those descriptors: code nothing reaches")
    for lo, hi, what in ((mbx, mbx_end, "mailbox"), (dat, dat_end, "data")):
        ok(lo >= rv + rms and hi <= page_end(rv + rms),
           "%s area 0x%08X..0x%08X is past the read-write segment's end (0x%08X), in its last page" %
           (what, lo, hi, rv + rms))
        br, wd, pr = refs_into(elf, lo, hi)
        ok(not wd and not pr, "no word and no lis/addi pair in the executable names the %s area" % what)

    # 7. the mailbox and the data image
    magic, version, size = struct.unpack(">IHH", box[0:8])
    ok(len(box) == hdr["MH_MBOX_SIZE"] and magic == box_defs["MBOX_MAGIC"] and
       version == box_defs["MBOX_VERSION"] and size == hdr["MH_MBOX_SIZE"],
       "the mailbox image: 'QMNU', version %d, %d bytes" % (version, size))
    ok(box[8] == hdr["MH_ROWS"] == 24 and box[9] == hdr["MH_ROW_BYTES"] == box_defs["MBOX_ROW_SIZE"] and
       box[10] == hdr["MH_MAIN_ROWS"] == 7 and box[11] == hdr["MH_CAPS"] and box[12] == 0 and box[13] == 0 and
       box[14] == 1 and box[15] == hdr["MH_CFG_ROWS"] == 24,
       "row_cap 24 (round 4), rows of 0x40, 7 rows a MAIN page (the main page's panes), 24 a "
       "CONFIG page, caps 0x%02X, closed, no request, renderer 1" % box[11])
    ok(box_defs["MBOX_OFF_ROWS"] + hdr["MH_ROWS"] * hdr["MH_ROW_BYTES"] == hdr["MH_MBOX_SIZE"] and
       all(b == 0 for b in box[16:]), "the rows fill the image, everything after the header zero")
    ok((hdr["MH_CAPS"] & box_defs["MBOX_CAP_PICKER"]) != 0,
       "caps has MBOX_CAP_PICKER: the root's Planets row opens the Galactic Map's look (mh_rac1_pick.c)")
    shim_off = hdr["MH_SHIM"] - hdr["MH_STATE"]
    ok(struct.unpack(">I", data[0:4])[0] == hdr["MH_MAGIC"] and
       all(b == 0 for b in data[4:shim_off + hdr["SH_SIZE"]]) and shim_off + hdr["SH_SIZE"] <= len(data),
       "the data image: the core's state at MH_STATE with its magic 'QMH1', the shim's (%d bytes) at MH_SHIM "
       "(+0x%X), everything else zero: the play hook sets the pages up" % (hdr["SH_SIZE"], shim_off))

    # 1. the originals, 2. the hook words
    order = ["MH_SITE_TEXT", "MH_SITE_PLAY", "MH_SITE_HELP_TEXT", "MH_SITE_HELP_PAGE"]
    ok(len(hooks) == 4 and [h[0] for h in hooks] == [hdr[k] for k in order],
       "four words: the text hook, the play hook, then the Help entry's two")
    for a, value, orig, what in hooks:
        ok(elf.word(a) == orig, "0x%08X holds 0x%08X in the executable (%s)" % (a, orig, what))
    stub_at = {}
    for a, value, orig, what in hooks:
        m = re.match(r"(b|bl) (mh_\w+_stub)", what)
        if m:
            link = m.group(1) == "bl"
            t = branch_target(a, value) if (value >> 26) == 18 and (value & 3) == (1 if link else 0) else None
            first = struct.unpack(">I", code[t - cave:t - cave + 4])[0] \
                if t is not None and cave <= t < cave + len(code) else None
            mask, want, name = STUB_FIRST_RAC1[m.group(2)]
            good = first is not None and (first & mask) == want
            ok(good, "0x%08X: %08X = %s 0x%08X, %s (%s)" % (a, value, m.group(1), t or 0, m.group(2), name))
            if good:
                stub_at[m.group(2)] = t
    ok(hooks[2][1] == hdr["MH_HELP_WORD"] and (hooks[2][1] >> 16) == hdr["R1_ID_TRAINER"] and
       (hooks[2][1] & 0xFFFF) == (hooks[2][2] & 0xFFFF) == 3,
       "the Help entry's text id 0x4EE6 becomes 0x7E00, \"Trainer\", its kind 3 (go to a page) kept")
    ok(hooks[3][1] == hdr["MH_SHIM"] + hdr["SH_PAGE_LIST"] and dat <= hooks[3][1] < dat + len(data),
       "the Help entry's page, Help's 0x72AF48, becomes the shim's MAIN page in the data image (0x%08X)" % hooks[3][1])

    # The sites in the game's code and data, and around them.
    W = elf.word
    ctx = [
        (0x00704D90, 0x0007B740, "get_ui_string's descriptor names 0x7B740, whose not-found path the text hook takes"),
        (0x0007B7C4, 0x4800000C, "the found path branches past the site to its blr ..."),
        (0x0007B7CC, 0x30630B94, "... and after the site, addic r3,r3,0xB94: 0x720B94, \"MESSAGE DOES NOT EXIST\""),
        (0x0007B7D0, 0x4E800020, "then blr: the stub returns as the function does"),
        (0x000E14CC, 0x38600001, "the play hook's site is followed by li r3,1 and a call"),
        (0x00729A80, 0x0000000E, "the main page's first pose, 0x0E (slot k: 0x0E + k) ..."),
        (0x00729A98, 0x00000014, "... its seventh, 0x14, then 7 .. 13"),
        (0x00729ABC, 0x00000002, "the main page is screen 2"),
        (0x00729AD4, 0x00729CF0, "its fifth item is 0x729CF0 ..."),
        (0x00729D24, 0x00729B68, "... whose entries are 0x729B68, the Help entry"),
        (0x00729CF0, 0x007075F0, "a one-row generic list: update 0x7075F0 ..."),
        (0x00729CF4, 0x007075F8, "... draw 0x7075F8, which the shim's rows name"),
        (0x00729D20, 0x00000084, "with flags 0x84, the shim's rows' (the large size)"),
        (0x00729BE8, 0x00000000, "the main page's first row has no up item: its links do not wrap"),
        (0x0072E1D8, 0x4FC60003, "Goodies' Cheats row: text 0x4FC6, kind 3 ..."),
        (0x0072E1DC, hdr["R1_CHEATS_PAGE"], "... to Cheats, 0x72F5D4: the CONFIG look"),
        (hdr["R1_CHEATS_PAGE"] + 0x00, hdr["R1_CHEATS_POSE0"], "Cheats' poses: 0x99 for its title ..."),
        (hdr["R1_CHEATS_PAGE"] + 0x04, hdr["R1_CHEATS_POSE0"] + 1, "0x9A its list pane ..."),
        (hdr["R1_CHEATS_PAGE"] + 0x08, hdr["R1_CHEATS_POSE0"] + 2, "0x9B its hint ..."),
        (hdr["R1_CHEATS_PAGE"] + 0x0C, hdr["R1_CHEATS_POSE0"] + 3, "0x9C and 0x9D two panes it gives no item ..."),
        (hdr["R1_CHEATS_PAGE"] + 0x10, hdr["R1_CHEATS_POSE0"] + 4, ""),
        (hdr["R1_CHEATS_PAGE"] + 0x14, 0x00000005, "then 5 .. 13, the others hidden, as the shim's pages have them"),
        (hdr["R1_CHEATS_PAGE"] + 0x34, 0x0000000D, ""),
        (hdr["R1_CHEATS_PAGE"] + 0x38, 0x0072E0E0, "its parent, Goodies"),
        (hdr["R1_CHEATS_PAGE"] + 0x3C, 0x00000026, "screen 38"),
        (hdr["R1_CHEATS_PAGE"] + 0x40, hdr["R1_CHEATS_LIST"], "its focus, the list item"),
        (hdr["R1_CHEATS_PAGE"] + 0x44, hdr["R1_CHEATS_TITLE"], "item 0, the title ..."),
        (hdr["R1_CHEATS_PAGE"] + 0x44 + 4 * hdr["R1_CONF_LIST_PANE"], hdr["R1_CHEATS_LIST"], "item 1, the list ..."),
        (hdr["R1_CHEATS_PAGE"] + 0x44 + 4 * hdr["R1_CONF_HINT_PANE"], hdr["R1_CHEATS_HINT"], "item 2, the hint ..."),
        (hdr["R1_CHEATS_PAGE"] + 0x50, 0x00000000, "and none on panes 3 and 4"),
        (hdr["R1_CHEATS_PAGE"] + 0x54, 0x00000000, ""),
        (hdr["R1_CHEATS_TITLE"] + 0x00, hdr["OPD_LABEL_UPDATE"], "the title item: the label's update and draw ..."),
        (hdr["R1_CHEATS_TITLE"] + 0x04, hdr["OPD_LABEL_DRAW"], ""),
        (hdr["R1_CHEATS_TITLE"] + 0x08, 0x00000000, "no enter ..."),
        (hdr["R1_CHEATS_TITLE"] + 0x30, 0x0000000F, "flags 0x0F ..."),
        (hdr["R1_CHEATS_TITLE"] + 0x34, 0x00004FC6, "text 0x4FC6, \"Cheats\": the shim's copy has its own id"),
        (hdr["R1_CHEATS_LIST"] + 0x00, 0x007077C0, "the list item: update 0x14A460 ..."),
        (hdr["R1_CHEATS_LIST"] + 0x04, 0x007077C8, "... draw 0x14A86C, whose geometry the shim's draw follows"),
        (0x007077C8, 0x0014A86C, ""),
        (hdr["R1_CHEATS_HINT"] + 0x04, 0x007075F8, "the hint, a generic list's draw ..."),
        (hdr["R1_CHEATS_HINT"] + 0x30, 0x00004002, "flags 0x4002 ..."),
        (hdr["R1_CHEATS_HINT"] + 0x34, 0x0072F6F4, "entries 0x72F6F4 ..."),
        (0x0072F6F4, 0x4EDF0000, "\"Toggle\" ..."),
        (0x0072F700, 0x4EE00000, "\"Exit\" ..."),
        (0x0072F70C, 0x00000000, "and the end"),
        (hdr["R1_MOVIES_PAGE"] + 0x00, hdr["R1_CONF_TITLE_POSE"], "In-Level Movies: its title pose 0xA4, the shim's pages' ..."),
        (hdr["R1_MOVIES_PAGE"] + 0x04, hdr["R1_CHEATS_POSE0"] + 1, "... and every other pose Cheats' ..."),
        (hdr["R1_MOVIES_PAGE"] + 0x08, hdr["R1_CHEATS_POSE0"] + 2, ""),
        (hdr["R1_MOVIES_PAGE"] + 0x0C, hdr["R1_CHEATS_POSE0"] + 3, ""),
        (hdr["R1_MOVIES_PAGE"] + 0x10, hdr["R1_CHEATS_POSE0"] + 4, ""),
        (hdr["R1_MOVIES_PAGE"] + 0x14, 0x00000005, ""),
        (hdr["R1_MOVIES_PAGE"] + 0x3C, 0x00000028, "screen 40"),
        (hdr["R1_MOVIES_PAGE"] + 0x44, 0x0072FAFC, "its title item ..."),
        (0x0072FAFC + 0x30, 0x0000000F, "flags 0x0F, as Cheats' ..."),
        (0x0072FAFC + 0x34, 0x00004FC8, "text 0x4FC8, \"In-Level Movies\", the longest title of the three pages"),
        (0x0072F768, 0x000000A3, "Cinematics: its own title pose ..."),
        (0x0072F76C, hdr["R1_CHEATS_POSE0"] + 1, "... and Cheats' list pane ..."),
        (0x0072F7B0, 0x0072FA24, "its list on pane 1 ..."),
        (0x0072FA28, 0x007076A8, "... a scroll list (draw 0x144638) ..."),
        (0x0072FA54, 0x00000400, "... with flags 0x400, no 0x10 ..."),
        (0x00144820, 0x546406F6, "which in the scroll draw means: flags & 0x10 ..."),
        (0x00144834, 0x335A0001, "or else n + 1 ..."),
        (0x00144838, 0x809F0024, "into the pane's height ..."),
        (0x0014483C, 0x7F44D3D6, "divided: Cinematics' rows share the list pane"),
        (0x0014A8A8, 0x38800005, "Cheats' list draw: the blend (1, 5, 4, ..) the shim's sets ..."),
        (0x0014A8B8, 0x38A00004, ""),
        (0x0014A9FC, 0x7C83DBD6, "the rows share the pane's height, divided by n + 1 ..."),
        (0x0014AA08, 0x3384FFF8, "the first row at that - 8 ..."),
        (0x0014AA24, 0x3C6080FF, "a row in 0x80FFA888 ..."),
        (0x0014AA34, 0x6063A888, ""),
        (0x0014AA40, 0x3C608020, "the cursor's in 0x8020FFFF ..."),
        (0x0014AA44, 0x6063FFFF, ""),
        (0x0014AA64, 0x3B00000C, "the label at x 12 ..."),
        (0x0014AA94, 0x809F0020, "the pane's width ..."),
        (0x0014AAA0, 0x3304FFF4, "the value right-aligned at w - 12"),
        (hdr["R1_CAMERA_LIST"] + 0x00, 0x007077D0, "Camera Options' list: update 0x14AB64 ..."),
        (hdr["R1_CAMERA_LIST"] + 0x04, 0x007077D8, "... draw 0x14AD88, whose 3D slider the shim's draw copies"),
        (0x007077D8, 0x0014AD88, ""),
        (0x0014AF00, 0x60A5A888, "its rows in Cheats' colours, 0x80FFA888 ..."),
        (0x0014AF1C, 0x3CA08020, "the cursor's 0x8020FFFF ..."),
        (0x0014AF20, 0x60A5FFFF, ""),
        (0x0014AED8, 0x33A4FFF8, "and the first row at h / (n + 1) - 8 as well"),
        # The planet picker (mh_rac1_pick.c): the Galactic Map's page and what the shim's copy keeps of it.
        (hdr["R1_MAP_PAGE"] + 0x00, hdr["R1_MAP_POSE0"], "the Galactic Map's poses: 0x63 for its title ..."),
        (hdr["R1_MAP_PAGE"] + 0x04, hdr["R1_MAP_POSE0"] + 1, "0x64 its list ..."),
        (hdr["R1_MAP_PAGE"] + 0x08, hdr["R1_MAP_POSE0"] + 2, "0x65 its galaxy ..."),
        (hdr["R1_MAP_PAGE"] + 0x0C, hdr["R1_MAP_POSE0"] + 3, "0x66 its hint ..."),
        (hdr["R1_MAP_PAGE"] + 0x10, hdr["R1_MAP_POSE0"] + 4, "0x67 its planet picture ..."),
        (hdr["R1_MAP_PAGE"] + 0x14, 0x00000005, "then 5 .. 13, the loader's pane among them, hidden"),
        (hdr["R1_MAP_PAGE"] + 0x34, 0x0000000D, ""),
        (hdr["R1_MAP_PAGE"] + 0x38, 0x00000000, "no parent: a root page, opened only by the opener ..."),
        (hdr["R1_MAP_PAGE"] + 0x3C, 0x0000000F, "screen 15, which makes every close travel (below)"),
        (hdr["R1_MAP_PAGE"] + 0x40, hdr["R1_MAP_LIST"], "its focus, the list"),
        (hdr["R1_MAP_PAGE"] + 0x44, hdr["R1_MAP_TITLE"], "items: the title ..."),
        (hdr["R1_MAP_PAGE"] + 0x48, hdr["R1_MAP_LIST"], "the list ..."),
        (hdr["R1_MAP_PAGE"] + 0x4C, hdr["R1_MAP_GALAXY"], "the galaxy ..."),
        (hdr["R1_MAP_PAGE"] + 0x50, hdr["R1_MAP_HINT"], "the hint ..."),
        (hdr["R1_MAP_PAGE"] + 0x54, hdr["R1_MAP_SHOT"], "the planet picture ..."),
        (hdr["R1_MAP_PAGE"] + 0x58, hdr["R1_MAP_LOADER"], "and the loader of planet maps, which the shim leaves out"),
        (hdr["R1_MAP_TITLE"] + 0x00, hdr["OPD_LABEL_UPDATE"], "the title, a label item ..."),
        (hdr["R1_MAP_TITLE"] + 0x04, hdr["OPD_LABEL_DRAW"], ""),
        (hdr["R1_MAP_TITLE"] + 0x08, 0x007077F0, "its enter 0x14B478, li r3,0: nothing ..."),
        (hdr["R1_MAP_TITLE"] + 0x34, 0x00004EF0, "text 0x4EF0, \"Galactic Map\""),
        (hdr["R1_MAP_LIST"] + 0x00, hdr["OPD_LIST_UPDATE"], "the map list: the generic update, which the shim's replaces ..."),
        (hdr["R1_MAP_LIST"] + 0x04, hdr["OPD_SCROLL_DRAW"], "... the scroll list's draw, which the shim's list keeps ..."),
        (hdr["R1_MAP_LIST"] + 0x08, 0x007077F8, "... its enter 0x14B480, which builds the discovered planets ..."),
        (hdr["R1_MAP_LIST"] + 0x30, hdr["R1_MAP_LIST_FLAGS"], "... flags 0x138, the shim's list's"),
        (0x007077F8, 0x0014B480, ""),
        (0x0014B4C4, 0x310852B0, "the enter's entries come from the planet table 0x7352B0 ..."),
        (0x0014B4E4, 0x314A3290, "into the buffer 0xA53290 ..."),
        (0x0014B53C, 0x61288000, "and it sets the list's flag 0x8000, the draw's first frame, as the shim's arrival does"),
        (hdr["R1_MAP_GALAXY"] + 0x00, 0x00707800, "the galaxy item's update 0x14B5B8 ..."),
        (0x00707800, 0x0014B5B8, ""),
        (0x0014B5E4, 0x3063DC78, "... which on the cross goes to the planet page 0x72DC78: the shim's copy has no update"),
        (hdr["R1_MAP_GALAXY"] + 0x04, hdr["OPD_GALAXY_DRAW"], "its draw, \"Galactic Map Draw\", the shim's copy's ..."),
        (hdr["OPD_GALAXY_DRAW"], 0x0014B5F4, ""),
        (0x004D29E4, 0x32105394, "which draws the planets from the table 0x735394 ..."),
        (0x004D2B20, 0x806E0224, "and labels the selected one, 0xA206E4"),
        (hdr["OPD_SCROLL_DRAW"], 0x00144638, "the scroll list's draw"),
        (hdr["R1_MAP_HINT"] + 0x04, hdr["OPD_LIST_DRAW"], "the hint, a generic list's draw ..."),
        (hdr["R1_MAP_HINT"] + 0x30, 0x00004002, "flags 0x4002 ..."),
        (hdr["R1_MAP_HINT"] + 0x34, 0x0072B1DC, "entries 0x72B1DC ..."),
        (0x0072B1DC, 0x4F050000, "\"Select\" ..."),
        (0x0072B1E8, 0x4EE00000, "\"Exit\" ..."),
        (0x0072B1F4, 0x00000000, "and the end"),
        (hdr["R1_MAP_SHOT"] + 0x00, 0x00707770, "the planet picture: update 0x1480F4 ..."),
        (hdr["R1_MAP_SHOT"] + 0x04, 0x00707778, "draw 0x1484A4 ..."),
        (hdr["R1_MAP_SHOT"] + 0x08, 0x00707780, "enter 0x148688 ..."),
        (hdr["R1_MAP_SHOT"] + 0x0C, 0x00707788, "exit 0x14876C ..."),
        (0x00707770, 0x001480F4, ""),
        (0x00707780, 0x00148688, ""),
        (0x00707788, 0x0014876C, ""),
        (hdr["R1_MAP_SHOT"] + 0x30, 0x00000007, "set 7, gmap_ss ..."),
        (hdr["R1_MAP_SHOT"] + 0x34, 0x00000022, "flag 2: the picture of the selected planet ..."),
        (0x00148160, 0x83C30224, "read from 0xA206E4"),
        (0x0072C0AC, 0x00707780, "the Options page's picture uses the same enter and exit, in an ordinary pause"),
        (0x0072C0B0, 0x00707788, ""),
        (hdr["R1_MAP_LOADER"] + 0x00, 0x00707620, "the loader of planet maps: update 0x142E38 ..."),
        (0x00707620, 0x00142E38, ""),
        (0x001430EC, 0x809D0040, "which loads a planet's map into the pause's base buffer (0xA10918) ..."),
        (0x001430F0, 0x483C0D11, "... with LoadAssetFromFile: only a pause opened on the map lays it out for that, so it is left out"),
        (0x0013B274, 0x2C87000F, "the opener's map-only memory: for screen 15 ..."),
        (0x0013B27C, 0x2C07000B, "... or 11 only"),
        (hdr["R1_MAP_INFO"] + 0x38, hdr["R1_MAP_PAGE"], "the planet page's parent is the map ..."),
        (hdr["R1_MAP_INFO"] + 0x3C, 0x00000010, "... screen 16, which travels too: left out"),
        (0x0013C03C, 0x80A30038, "the transition: the current page's parent ..."),
        (0x0013C040, 0x7C852010, "... against the next page ..."),
        (0x0013C044, 0x7C9F0034, "... equal: back to the parent"),
        (0x0013C10C, 0x80790004, "back: the current page's ..."),
        (0x0013C114, 0x7C83E82E, "... pose for each pane ..."),
        (0x0013C13C, 0x30A5FFFF, "... at its last frame ..."),
        (0x0013C14C, 0x3C60BF80, "... played at -1.0 (the picker's open and its way back) ..."),
        (0x0013C150, 0x90640058, ""),
        (0x0013C164, 0x7C83E82E, "forwards: the next page's pose ..."),
        (0x0013C15C, 0x63850000, "... from frame 0 ..."),
        (0x0013C17C, 0x3C803F80, "... at 1.0"),
        (0x0013C20C, 0x38800001, "a close is action 1 ..."),
        (0x0013C210, 0x2C03000F, "... but on screen 15 ..."),
        (0x0013C218, 0x2C030010, "... or 16 ..."),
        (0x0013C220, 0x38800002, "... action 2, travel: the picker's page is screen 50"),
        (0x0014AF68, 0x33BD0006, "the 3D slider: the rows after it 6 lower ..."),
        (0x0014AFDC, 0x307A0057, "its frame from w / 2 + 87 ..."),
        (0x0014AFD4, 0x30C4FFF6, "to w - 10 ..."),
        (0x0014AFD8, 0x60A96969, "in 0x80696969 ..."),
        (0x0014B008, 0x309A0059, "its track from w / 2 + 89 ..."),
        (0x0014B01C, 0x30A3FFF4, "to w - 12 ..."),
        (0x0014B020, 0x60C93838, "in 0x80383838 ..."),
        (0x0014B070, 0x6083E99E, "the fill: sprite group 0xE99E ..."),
        (0x0014B078, 0x38800008, "frame 8 ..."),
        (0x0014B0B8, 0x39200013, "v0 19 ..."),
        (0x0014B0BC, 0x39400035, "u1 53 ..."),
        (0x0014B088, 0x3960002B, "v1 43 ..."),
        (0x0014B090, 0x39800080, "alpha 128"),
        (0x001420E0, 0x635A3030, "the generic list's grey row, 0x80303030"),
        (0x0006FDEC, 0x3D400073, "text(): the medium font ..."),
        (0x0006FDF0, 0x314A68B0, "0x7368B0, which the shim measures with"),
    ]
    for a, want, what in ctx:
        ok(W(a) == want, "0x%08X holds 0x%08X%s" % (a, want, (": " + what) if what else ""))
    e = W(0x0072FA24 + 0x34)
    rows = 0
    while rows < 64 and (W(e + 12 * rows) >> 16) != 0:
        rows += 1
    ok(rows == 29, "Cinematics' list has %d rows, all sharing Cheats' list pane: the pane the shim's CONFIG "
                   "pages draw in is a tall one" % rows)
    ok(hdr["R1_CONF_LIST_PANE"] == 1 and hdr["R1_CONF_HINT_PANE"] == 2 and hdr["R1_CFG_ROW_H"] == 20 and
       hdr["R1_CFG_VIS"] == 8,
       "the shim's list on pane 1 and Cheats' hint on pane 2, rows of 20 units at least, eight before the pane "
       "is measured")
    names = [W(hdr["R1_PLANET_TABLE"] + 12 * k) for k in range(20)]
    subs = [W(hdr["R1_PLANET_TABLE"] + 12 * k + 4) for k in range(20)]
    ok(names[:19] == [0x4EB9 + k for k in range(19)] and names[19] == 0 and all(0x4ECD <= x <= 0x4EDE for x in subs[:19])
       and hdr["R1_PLANETS"] == 19 and hdr["R1_VELDIN2"] == 18,
       "the planet table: nineteen planets, names 0x4EB9 .. 0x4ECB and their subtitles, then none: qwark's "
       "PLANET_LIST, Veldin 0 .. Veldin2 18, which the picker lists in that order")
    ok(R1_ID_OK(hdr), "the picker's own text id, Veldin2's, is in the text stub's range")
    br, wd, pr = refs_into(elf, hdr["MH_SITE_HELP_TEXT"], hdr["MH_SITE_HELP_TEXT"] + 0x18)
    ok([a for a, _ in wd] == [0x00729D24] and not pr,
       "nothing but the main page's Help item names the Help entry: no other list changes")

    # 4. the code, 5. the descriptors
    bad = []
    for lo, _, c, _ in areas:
        bad += lint_code(c, lo, allowed=in_code)
    ok(not bad, "the code: every branch stays in the renderer's four areas, no sc, no vector instruction, no write to r13")
    for b in bad[:12]:
        print("        " + b)
    opds = dict((v, k) for k, v in hdr.items() if k.startswith("OPD_") and not k.endswith("#comment"))
    handed = {hdr["OPD_LIST_DRAW"], hdr["OPD_LABEL_UPDATE"], hdr["OPD_LABEL_DRAW"], hdr["OPD_SCROLL_DRAW"],
              hdr["OPD_GALAXY_DRAW"]}
    seen, bad = set(), []
    for lo, _, c, _ in areas:
        b, s = calls_via_descriptors(c, lo, set(opds), elf, toc=RAC1_TOC, code_hi=0x700000)
        bad += b
        seen |= s
    ok(not bad, "every indirect call switches r2 through one of the header's descriptors and back")
    for b in bad[:12]:
        print("        " + b)
    ok(set(opds) - handed <= seen,
       "and every descriptor the header names for a call is one the code builds (%d of %d)" %
       (len(seen & (set(opds) - handed)), len(set(opds) - handed)))
    for name in sorted(opds.values()):
        a = hdr[name]
        m = re.search(r"(0x[0-9A-Fa-f]+)\s*$", hdr.get(name + "#comment", ""))
        fn = int(m.group(1), 16) if m else None
        ok(fn is not None and elf.word(a) == fn and elf.word(a + 4) == RAC1_TOC,
           "%s 0x%08X = {0x%08X, TOC 0x%08X}" % (name, a, fn or 0, RAC1_TOC))

    # 8. the leaf stubs, stepped
    if all(k in stub_at for k in ("mh_text_stub", "mh_play_stub")):
        try:
            check_rac1_stubs(ok, code, cave, hdr, stub_at, in_code)
        except ValueError as e:
            ok(False, "the stubs step: %s" % e)
    else:
        ok(False, "both stubs found to step")

    # 9. overlaps
    ours = [(lo, lo + len(c), "renderer code") for lo, _, c, _ in areas]
    ours += [(mbx, mbx + len(box), "renderer mailbox"), (dat, dat + len(data), "renderer data")]
    ours += [(a, a + 4, "renderer word") for a, _, _, _ in hooks]
    known = mod_entries(roots, "NPEA00385")
    clashes, accepted = [], []
    for lo, hi, what in ours:
        for a, b, label, mod, kind in known:
            if a < hi and b > lo:
                text = "%s 0x%08X..0x%08X vs %s/%s %s 0x%08X..0x%08X" % (what, lo, hi, label, mod, kind, a, b)
                (accepted if mod in ACCEPTED_RAC1 and what == "renderer word" else clashes).append((text, mod))
        for a, b, w in QWARK_RAC1:
            if a < hi and b > lo:
                clashes.append(("%s 0x%08X..0x%08X vs qwark %s 0x%08X..0x%08X" % (what, lo, hi, w, a, b), None))
    ok(not clashes, "no overlap with %d mod words and caves (the probes', charge-boots' among them), nor with qwark's "
       "%d own (the savefile helper's caves and hook, the code switches)" % (len(known), len(QWARK_RAC1)))
    for c, _ in clashes:
        print("        " + c)
    named = []
    for lo, hi in [(lo, hi) for lo, hi, _, _ in areas] + [(mbx, mbx_end), (dat, dat_end)]:
        named += mod_pointers(roots, "NPEA00385", lo, hi)
    ok(not named, "no mod's code names an address in any of the six areas")
    for x in named[:12]:
        print("        " + x)
    seen_acc = set()
    for c, mod in accepted:
        if mod not in seen_acc:
            print("        known: %s/...: %s" % (mod, ACCEPTED_RAC1[mod]))
            seen_acc.add(mod)
    print("        (%d words shared with the prototypes by design)" % len(accepted))


def parse_descs(path):
    """Each renderer's (nhooks, nhelp) from menuhelper_bins.c's mh_descs, by game ("rac4" ..)."""
    text = open(path, encoding="utf-8").read()
    out = {}
    for d in re.finditer(r"\{ \d+, mh_(\w+)_caves, \d+, mh_\w+_hooks, (\d+), (\d+),", text):
        out[d.group(1)] = (int(d.group(2)), int(d.group(3)))
    return out


def check_help_rows(ok):
    """Revision 1.23: each renderer's Help row words, which menu.help swaps on their own."""
    text = open(GEN_C, encoding="utf-8").read()
    descs = parse_descs(GEN_C)
    for g, name in (("rac4", "Deadlocked"), ("rac3", "UYA"), ("rac2", "RaC2"), ("rac1", "RaC1")):
        hdr = header_defs(os.path.join(HERE, "mh_%s.h" % g))
        sites = sorted(v for k, v in hdr.items() if k.startswith("MH_SITE_HELP") and not k.endswith("#comment"))
        _, data, _, hooks, caves, _ = parse_bins(GEN_C, g)
        if g not in descs or not hooks or not sites:
            ok(False, "%s: the desc, the hook words and the Help sites are there to read" % name)
            continue
        nhooks, nhelp = descs[g]
        last = [h[0] for h in hooks[len(hooks) - nhelp:]] if nhelp else []
        ok(nhooks == len(hooks) and nhelp == len(sites) and sorted(last) == sites and
           not [h for h in hooks[:len(hooks) - nhelp] if h[0] in sites],
           "%s: the Help row is the table's last %d words (%s), every MH_SITE_HELP site and no other word" %
           (name, nhelp, ", ".join("0x%08X" % a for a in last)))
        named = []
        for addr, cname, code, _ in caves:
            if not code:
                continue
            m = re.search(r"static const u8 %s\[\] = \{(.*?)\};" % cname, text, re.S)
            body = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))) if m else b""
            for a in sites:
                named += code_pointers(body, addr, a, a + 4)
        held = []
        if data is not None:
            for i in range(0, len(data) - 3, 4):
                if struct.unpack(">I", data[i:i + 4])[0] in sites:
                    held.append(i)
        ok(not named and not held,
           "%s: no code of the renderer names a Help site and no word of its data holds one, so it never reads "
           "them and works the same with the Help row in or out" % name)
        for x in named[:8]:
            print("        " + x)


def shim_set_layout(shim_defs, rows):
    """Text set 0's first row, from the state, and a set's size, for `rows` mailbox rows (mh_shim.h)."""
    set_flags = shim_defs["SET_KIND"] + rows
    set_id = (set_flags + rows + 1) & ~1
    set_title = (set_id + 2 * rows + 3) & ~3
    set_text = set_title + shim_defs["MH_TITLE_LEN"]
    return shim_defs["ST_SET0"] + set_text, set_text + rows * shim_defs["MH_TEXT_LEN"]


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
    ap.add_argument("--bonus", default=None,
                    help="Deadlocked's bonus.ps3 (its IGE layouts), read for templates 37 and 28")
    args = ap.parse_args()
    bonus = args.bonus or os.path.join(os.path.expanduser("~"), DL_BONUS)

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
    check_rac4(ok, Elf(os.path.join(eboots, "NPEA00423.elf")), roots, bonus)
    check_rac3(ok, Elf(os.path.join(eboots, "NPEA00387.elf")), roots)
    check_rac2(ok, Elf(os.path.join(eboots, "NPEA00386.elf")), roots)
    check_rac1(ok, Elf(os.path.join(eboots, "NPEA00385.elf")), roots)
    check_help_rows(ok)

    print("\n%s" % ("all checks passed" if failures == 0 else "%d check(s) FAILED" % failures))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
