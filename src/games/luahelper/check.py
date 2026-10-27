#!/usr/bin/env python3
"""
check.py: checks the Lua helper against the executable it goes into, offline,
with no SDK and nothing running (the Lua counterpart of menuhelper/check.py).

    python -I src/games/luahelper/check.py [--elf PATH]
    python -I src/games/luahelper/check.py --yml PPU-<hash>   also print the RPCS3 patch

PATH defaults to C:/Users/atune/Documents/ToD_lunacy/EBOOT.ELF, the NPEA00452
v01.00 executable (md5 df9d68ada99b3f44ffe798ce03a1549c).

What it checks, all against the file:
  - the executable is the one the helper was built for (md5, fingerprint);
  - both hook sites hold the words the helper replaces, the VM hook's call
    goes to CGameplayLuaVM_update with r3 loaded from the TOC slot that holds
    g_gameplayLuaVM and a nop after it, and the error tap's word is reached
    from both VMs' vtables;
  - every game function the helper calls has a descriptor in .opd whose TOC is
    the one the helper calls it with, and the few whose bodies matter
    (LuaAlloc_setCurrent, luaL_loadbuffer, lua_pcall) do what the helper
    assumes;
  - both caves are outside every segment's bytes but inside its last 64 KB
    page, and nothing in the executable refers to them;
  - the helper in luahelper_bins.c fits its cave, both hook words reach the
    entries they should, and every branch out of the helper lands on one of
    the functions above.
Exit status 0 when all pass.
"""
import argparse
import hashlib
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
QWARK = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
HDR = os.path.join(HERE, "lh_tod.h")
LUAMB_H = os.path.join(QWARK, "src", "core", "luamb.h")
BINS_C = os.path.join(QWARK, "src", "games", "luahelper_bins.c")
DEFAULT_ELF = "C:/Users/atune/Documents/ToD_lunacy/EBOOT.ELF"
MD5 = "df9d68ada99b3f44ffe798ce03a1549c"

FINGERPRINT = (0x002EA5F0, 0x93EB06E8)
NOP = 0x60000000
BLR = 0x4E800020
PAGE = 0x10000

# The three TOC ranges (REPORT-tod-lua section 1).
RANGES = ((0x10200, 0x2670A0, 0x8AB078), (0x2670A0, 0x567898, 0x8BB068), (0x567898, 0x864280, 0x8CB054))

# Known functions used by the checks below.
LUA_LOAD = 0x6913E4
LUAD_PCALL = 0x69A198
GAMEPLAY_REPORT_ERROR = 0x290B70
VTBL_CLUAVM = 0x100FC5D8
VTBL_CGAMEPLAYLUAVM = 0x1006C898
VTBL_REPORT_ERROR = 0x18


def defines(path):
    out = {}
    for line in open(path, encoding="ascii"):
        m = re.match(r"\s*#define\s+(\w+)\s+(0x[0-9A-Fa-f]+|\d+)\b", line)
        if m:
            out[m.group(1)] = int(m.group(2), 0)
    return out


class Elf:
    def __init__(self, data):
        self.d = data
        phoff = struct.unpack(">Q", data[0x20:0x28])[0]
        phsz, phn = struct.unpack(">HH", data[0x36:0x3A])
        self.load = []
        self.tls = None
        for i in range(phn):
            p = data[phoff + i * phsz: phoff + (i + 1) * phsz]
            t, f, off, va, pa, fs, ms, al = struct.unpack(">IIQQQQQQ", p)
            if t == 1 and ms:
                self.load.append((va, fs, ms, off, f))
            if t == 7:
                self.tls = (va, ms)

    def seg(self, a):
        for va, fs, ms, off, f in self.load:
            if va <= a < va + ms:
                return va, fs, ms, off, f
        return None

    def u32(self, a):
        s = self.seg(a)
        if s is None:
            return None
        va, fs, ms, off, f = s
        if a + 4 > va + fs:
            return 0
        return struct.unpack(">I", self.d[off + a - va: off + a - va + 4])[0]


def branch_target(word, at):
    op = word >> 26
    if op != 18:
        return None
    li = word & 0x03FFFFFC
    if li & 0x02000000:
        li -= 0x04000000
    return (li if word & 2 else at + li), bool(word & 1)


def toc_for(a):
    for lo, hi, r2 in RANGES:
        if lo <= a < hi:
            return r2
    return None


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


class Report:
    def __init__(self):
        self.fails = 0

    def check(self, ok, what):
        print(("  ok    " if ok else "  FAIL  ") + what)
        if not ok:
            self.fails += 1


def helper_bytes():
    """The cave bytes, hook words and descriptor out of luahelper_bins.c."""
    src = open(BINS_C, encoding="ascii").read()
    m = re.search(r"static const u8 lh_tod_text\[\] = \{(.*?)\};", src, re.S)
    text = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))
    hooks = [tuple(int(v, 16) for v in h) for h in
             re.findall(r"\{ 0x([0-9A-Fa-f]+)u, 0x([0-9A-Fa-f]+)u, 0x([0-9A-Fa-f]+)u \}", src)]
    cave = int(re.search(r"\{ 0x([0-9A-Fa-f]+)u, lh_tod_text", src).group(1), 16)
    return cave, text, hooks


def find_opds(elf, entries):
    """address -> list of (opd address, toc) for every .opd-shaped pair naming it."""
    out = {e: [] for e in entries}
    for va, fs, ms, off, f in elf.load:
        if f & 1:            # executable segment: not where descriptors are
            continue
        for a in range(va, va + fs - 4, 4):
            w = elf.u32(a)
            if w in out:
                out[w].append((a, elf.u32(a + 4)))
    return out


def refs_into(elf, lo, hi):
    """Every word in the file whose value is in [lo, hi), and every lis/addi or
    lis/ori pair in the code that forms one."""
    hits = []
    for va, fs, ms, off, f in elf.load:
        for a in range(va, va + fs - 3, 4):
            w = struct.unpack(">I", elf.d[off + a - va: off + a - va + 4])[0]
            if lo <= w < hi:
                hits.append(("word", a, w))
    va, fs, ms, off, f = elf.load[0]
    code = elf.d[off: off + fs]
    for i in range(0, fs - 4, 4):
        w = struct.unpack(">I", code[i:i + 4])[0]
        if w >> 26 == 15 and (w >> 16) & 31 == 0:          # lis rD, hi
            rd = (w >> 21) & 31
            hi16 = (w & 0xFFFF) << 16
            for j in range(i + 4, min(i + 40, fs - 4), 4):
                y = struct.unpack(">I", code[j:j + 4])[0]
                op = y >> 26
                ra = (y >> 16) & 31
                if op in (14, 32, 34, 36, 38, 40, 44, 48, 52, 58, 62) and ra == rd:    # addi / d-form loads, stores
                    v = (hi16 + s16(y & 0xFFFF)) & 0xFFFFFFFF
                elif op == 24 and (y >> 21) & 31 == rd:                                  # ori rA, rS
                    v = hi16 | (y & 0xFFFF)
                else:
                    continue
                if lo <= v < hi:
                    hits.append(("pair", va + i, v))
                break
    return hits


def toc_refs_into(elf, lo, hi):
    """Every r2-relative load, store or addi in the code whose address, with its
    range's r2, is in [lo, hi). `addi r2,r2,x` is left out: that is a
    cross-TOC trampoline, which runs with its caller's r2."""
    out = []
    va, fs, ms, off, f = elf.load[0]
    for i in range(0, fs - 4, 4):
        w = struct.unpack(">I", elf.d[off + i: off + i + 4])[0]
        op = w >> 26
        if op not in (14, 32, 34, 36, 38, 40, 44, 48, 52, 58, 62) or (w >> 16) & 31 != 2:
            continue
        if op == 14 and (w >> 21) & 31 == 2:
            continue
        r2 = toc_for(va + i)
        if r2 is None:
            continue
        ea = (r2 + s16(w & 0xFFFF)) & 0xFFFFFFFF
        if lo <= ea < hi:
            out.append((va + i, ea))
    return out


def patch_words(text, cave, hooks, h, mb):
    words = []
    for at in range(0, len(text), 4):
        words.append((cave + at, struct.unpack(">I", (text[at:at + 4] + b"\0\0\0")[:4])[0]))
    words.append((h["LH_MAILBOX"], mb["LUAMB_MAGIC"]))
    words.append((h["LH_MAILBOX"] + 4, (mb["LUAMB_VERSION"] << 16) | mb["LUAMB_SIZE"]))
    for addr, value, orig in reversed(hooks):
        words.append((addr, value))
    return words


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=DEFAULT_ELF)
    ap.add_argument("--yml", metavar="PPU-HASH", help="also print the RPCS3 patch for this executable hash")
    args = ap.parse_args()

    h = defines(HDR)
    mb = defines(LUAMB_H)
    data = open(args.elf, "rb").read()
    elf = Elf(data)
    r = Report()

    print("the executable")
    r.check(hashlib.md5(data).hexdigest() == MD5, "md5 is %s (NPEA00452 v01.00)" % MD5)
    r.check(elf.u32(FINGERPRINT[0]) == FINGERPRINT[1], "fingerprint 0x%08X = 0x%08X" % FINGERPRINT)

    print("the VM hook")
    site = h["LH_HOOK_ADDR"]
    w = elf.u32(site)
    r.check(w == h["LH_HOOK_ORIG"], "0x%X holds 0x%08X" % (site, h["LH_HOOK_ORIG"]))
    bt = branch_target(w, site)
    r.check(bt == (h["LH_FN_VM_UPDATE"], True), "it is bl CGameplayLuaVM_update (0x%X)" % h["LH_FN_VM_UPDATE"])
    r.check(elf.u32(site + 4) == h["LH_HOOK_NEXT"], "a nop follows it, so the caller expects r2 back as it was")
    prev = elf.u32(site - 4)
    r2 = toc_for(site)
    r.check(r2 == h["LH_GAME_TOC"], "the site's TOC range is r2 = 0x%X" % h["LH_GAME_TOC"])
    loads_vm = prev >> 26 == 32 and (prev >> 21) & 31 == 3 and (prev >> 16) & 31 == 2
    slot = (r2 + s16(prev & 0xFFFF)) & 0xFFFFFFFF if loads_vm else None
    r.check(loads_vm and elf.u32(slot) == h["LH_VM"],
            "r3 comes from lwz r3,(r2) slot %s, which holds g_gameplayLuaVM 0x%X"
            % (hex(slot) if slot else "?", h["LH_VM"]))
    r.check(toc_for(h["LH_FN_VM_UPDATE"]) == h["LH_GAME_TOC"],
            "CGameplayLuaVM_update is in the same TOC range, so the entry calls it with the caller's r2")

    print("the error tap")
    es = h["LH_ERR_HOOK_ADDR"]
    r.check(elf.u32(es) == h["LH_ERR_HOOK_ORIG"], "0x%X holds blr (CLuaVM_reportError is empty)" % es)
    opds = find_opds(elf, [es, GAMEPLAY_REPORT_ERROR])
    base_slot = elf.u32(VTBL_CLUAVM + VTBL_REPORT_ERROR)
    gp_slot = elf.u32(VTBL_CGAMEPLAYLUAVM + VTBL_REPORT_ERROR)
    r.check(base_slot is not None and elf.u32(base_slot) == es,
            "CLuaVM's vtable slot 0x18 is the descriptor of 0x%X" % es)
    r.check(gp_slot is not None and elf.u32(gp_slot) == GAMEPLAY_REPORT_ERROR,
            "CGameplayLuaVM's slot 0x18 is CGameplayLuaVM_reportError 0x%X" % GAMEPLAY_REPORT_ERROR)
    reaches = False
    for a in range(GAMEPLAY_REPORT_ERROR, GAMEPLAY_REPORT_ERROR + 0x24, 4):
        t = branch_target(elf.u32(a), a)
        if t and t[1]:
            tramp = t[0]
            tt = branch_target(elf.u32(tramp + 12), tramp + 12)
            reaches = elf.u32(tramp) == 0xF8410028 and tt == (es, False)
            break
    r.check(reaches, "CGameplayLuaVM_reportError calls a cross-TOC trampoline that branches to 0x%X" % es)

    print("the functions the helper calls")
    fns = {k: v for k, v in h.items() if k.startswith("LH_FN_")}
    opds = find_opds(elf, list(fns.values()))
    for name, addr in sorted(fns.items(), key=lambda kv: kv[1]):
        want = h["LH_GAME_TOC"] if name == "LH_FN_VM_UPDATE" else h["LH_LUA_TOC"]
        tocs = sorted({t for _, t in opds[addr]})
        r.check(toc_for(addr) == want and tocs == [want],
                "%s 0x%X: descriptor TOC %s, called with 0x%X"
                % (name, addr, ",".join("0x%X" % t for t in tocs) or "none", want))
    # LuaAlloc_setCurrent: lwz r9,X(r2); stw r3,0(r9); blr, with the TOC slot holding the allocator global.
    a = h["LH_FN_ALLOC_SET_CURRENT"]
    w0, w1, w2 = elf.u32(a), elf.u32(a + 4), elf.u32(a + 8)
    ok = w0 >> 26 == 32 and (w0 >> 16) & 31 == 2 and w1 >> 26 == 36 and (w1 & 0xFFFF) == 0 and w2 == BLR
    slot = (h["LH_LUA_TOC"] + s16(w0 & 0xFFFF)) & 0xFFFFFFFF
    r.check(ok and elf.u32(slot) == h["LH_CUR_ALLOC"],
            "LuaAlloc_setCurrent stores r3 into the word at 0x%X (LH_CUR_ALLOC)" % h["LH_CUR_ALLOC"])
    a = h["LH_FN_LOADBUFFER"]
    calls = [branch_target(elf.u32(x), x) for x in range(a, a + 0x30, 4)]
    r.check((LUA_LOAD, True) in calls, "luaL_loadbuffer calls lua_load 0x%X" % LUA_LOAD)
    a = h["LH_FN_PCALL"]
    calls = [branch_target(elf.u32(x), x) for x in range(a, a + 0x200, 4)]
    r.check((LUAD_PCALL, True) in calls, "lua_pcall calls luaD_pcall 0x%X" % LUAD_PCALL)

    print("the caves")
    va0, fs0, ms0, _, f0 = elf.load[0]
    cave, cave_end = h["LH_CAVE"], h["LH_CAVE_END"]
    r.check(f0 & 1 and va0 + ms0 <= cave and cave_end <= (va0 + ms0 + PAGE - 1) // PAGE * PAGE,
            "code cave 0x%X..0x%X is past the code segment's end 0x%X and inside its last page"
            % (cave, cave_end, va0 + ms0))
    r.check(all(elf.seg(x) is None for x in (cave, cave_end - 4)), "no segment has bytes there")
    rw = [s for s in elf.load if s[4] == 6 and s[0] < 0x10000000][0]
    mbx, mbx_end = h["LH_MAILBOX"], h["LH_MAILBOX_END"]
    tls_end = elf.tls[0] + elf.tls[1] if elf.tls else 0
    r.check(rw[0] + rw[2] <= mbx and tls_end <= mbx and mbx_end <= (rw[0] + rw[2] + PAGE - 1) // PAGE * PAGE,
            "mailbox 0x%X..0x%X is past the RW segment (0x%X) and the TLS image (0x%X), inside the page"
            % (mbx, mbx_end, rw[0] + rw[2], tls_end))
    r.check(mbx + mb["LUAMB_SIZE"] <= mbx_end, "LUAMB_SIZE 0x%X fits" % mb["LUAMB_SIZE"])
    # What the helper occupies. A word that only looks like an address there
    # is data when it is not 4-aligned (code), or when it is a descriptor's TOC
    # word: the top range's r2, 0x8CB054, lies inside the RW page itself, and
    # every one of its descriptors says so. What r2 reaches is checked apart.
    used = len(helper_bytes()[1])
    tocs = {t for _, _, t in RANGES}
    hits = [x for x in refs_into(elf, cave, cave + used) if x[2] % 4 == 0]
    hits += [x for x in refs_into(elf, mbx, mbx + mb["LUAMB_SIZE"]) if not (x[0] == "word" and x[2] in tocs)]
    r.check(not hits, "nothing in the executable refers to the helper's bytes or the mailbox"
            + ("" if not hits else ": " + ", ".join("%s at 0x%X -> 0x%X" % x for x in hits[:8])))
    r2_hits = toc_refs_into(elf, rw[0] + rw[2] - (rw[0] + rw[2]) % 0x200, mbx_end)
    r.check(not r2_hits, "no TOC-relative access reaches past the TOC (0x%X..)" % (rw[0] + rw[2])
            + ("" if not r2_hits else ": " + ", ".join("0x%X -> 0x%X" % x for x in r2_hits[:8])))

    print("the helper (luahelper_bins.c)")
    bcave, text, hooks = helper_bytes()
    r.check(bcave == cave, "it is linked at the cave 0x%X" % cave)
    r.check(cave + len(text) <= cave_end, "%d bytes, which fit (0x%X spare)" % (len(text), cave_end - cave - len(text)))
    hk = {addr: (value, orig) for addr, value, orig in hooks}
    vm_word, vm_orig = hk.get(site, (0, 0))
    r.check(vm_orig == h["LH_HOOK_ORIG"] and branch_target(vm_word, site) == (cave, True),
            "the VM hook word 0x%08X is bl to the cave's first byte" % vm_word)
    err_word, err_orig = hk.get(es, (0, 0))
    et = branch_target(err_word, es)
    r.check(err_orig == h["LH_ERR_HOOK_ORIG"] and et is not None and not et[1] and cave <= et[0] < cave + len(text),
            "the error tap word 0x%08X is b (no link) into the helper" % err_word)
    allowed = set(fns.values())
    bad = []
    for at in range(0, len(text), 4):
        w = struct.unpack(">I", text[at:at + 4])[0]
        t = branch_target(w, cave + at)
        if t and not (cave <= t[0] < cave + len(text)) and t[0] not in allowed:
            bad.append((cave + at, t[0]))
    r.check(not bad, "every branch out of the helper reaches an LH_FN_ function"
            + ("" if not bad else ": " + ", ".join("0x%X -> 0x%X" % x for x in bad)))
    toc_sets = [at for at in range(0, len(text) - 4, 4)
                if text[at:at + 4] == struct.pack(">I", 0x3C400000 | ((h["LH_LUA_TOC"] + 0x8000) >> 16))]
    r.check(len(toc_sets) == 1, "the entry sets r2 to 0x%X once, before lh_tick" % h["LH_LUA_TOC"])

    if args.yml:
        words = patch_words(text, cave, hooks, h, mb)
        print()
        print("# RPCS3 patch for NPEA00452 v01.00: the Lua helper (%d words). Put it in" % len(words))
        print("# patches/imported_patch.yml or a patch file of your own; RPCS3 applies it at load.")
        print("%s:" % args.yml)
        print("  \"qwark Lua helper\":")
        print("    Games:")
        print("      \"Ratchet & Clank Future: Tools of Destruction\":")
        print("        NPEA00452: [ 01.00 ]")
        print("    Author: qwark")
        print("    Notes: \"The in-game Lua mailbox (docs/tod-poc.md).\"")
        print("    Patch Version: 1.0")
        print("    Patch:")
        for addr, value in words:
            print("      - [ be32, 0x%08X, 0x%08X ]" % (addr, value))

    print()
    print("%s: %s" % ("check.py", "all passed" if r.fails == 0 else "%d FAILED" % r.fails))
    return 0 if r.fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
