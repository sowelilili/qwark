#!/usr/bin/env python3
"""
test_pkgdata.py: pkg.py's library of the client's mods and premade savefiles,
held to the client's rules and to the installer's C code.

    python -I installer/test/test_pkgdata.py installer/test/installer-test.exe

installer/test/run.sh runs it after building installer-test.exe, whose
--expand mode is qrle.c's expansion and whose --install mode is one inst_run
over a fake console: so pkg.py's encoder is held to the decoder the console
runs, and a library pkg.py makes from a client checkout is installed by the
console's own code and read back off the fake HDD. Needs no SDK.
"""
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import types
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
INSTALLER = os.path.dirname(HERE)
QWARK = os.path.dirname(INSTALLER)
sys.path.insert(0, INSTALLER)
sys.dont_write_bytecode = True
import pkg  # noqa: E402

EXE = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(HERE, "installer-test.exe")
TIMEOUT = 120

passed = failed = 0


def check(ok, what):
    global passed, failed
    if ok:
        passed += 1
    else:
        failed += 1
        print("FAIL %s" % what)


def group(name):
    print("-- %s" % name)


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode("utf-8"))


def c_expand(work, stream, size):
    """qrle.c's expansion of stream, or None when it refuses it."""
    src, out = os.path.join(work, "in.qrle"), os.path.join(work, "out.bin")
    write(src, stream)
    rc = subprocess.run([EXE, "--expand", src, str(size), out], timeout=TIMEOUT).returncode
    if rc == 3:
        return None
    if rc != 0:
        raise SystemExit("installer-test.exe --expand failed (%d)" % rc)
    return open(out, "rb").read()


# ---------------------------------------------------------------- qrle

def test_qrle(work):
    group("qrle: pkg.py's encoder against qrle.c's expansion")
    rnd = random.Random(1972)
    save_like = bytearray(2 << 20)
    save_like[:3000] = bytes(rnd.randrange(256) for _ in range(3000))
    save_like[900000:1000000] = b"\xff" * 100000
    save_like[1500000:1500040] = bytes(range(40))
    cases = {
        "nothing": b"",
        "one byte": b"\x07",
        "a run of three, kept as a literal": b"abbbc",
        "a run of four": b"abbbbc",
        "runs at both ends": b"\0" * 9 + b"xyz" + b"\xff" * 9,
        "a run of 64, a varint's first byte": b"\x01" * 64,
        "a run of 65, a varint's second byte": b"\x01" * 65,
        "a literal of 64 and of 65": bytes(range(64)) + b"\x02" * 5 + bytes(range(65)),
        "a run past 2^21, a varint of four bytes": b"\0" * ((1 << 21) + 5),
        "random bytes, no runs to speak of": bytes(rnd.randrange(256) for _ in range(70000)),
        "random bytes over a small alphabet, short runs everywhere": bytes(rnd.choice(b"\0\0\0\1") for _ in range(50000)),
        "a savefile's shape": bytes(save_like),
    }
    for name, data in cases.items():
        stream = pkg.qrle_encode(data)
        check(pkg.qrle_decode(stream, len(data)) == data, "%s: pkg.py decodes its own encoding" % name)
        check(c_expand(work, stream, len(data)) == data, "%s: qrle.c expands pkg.py's encoding to the same bytes" % name)

    check(len(pkg.qrle_encode(bytes(2 << 20))) <= 6, "2 MB of zeros is one run token")
    check(len(pkg.qrle_encode(bytes(save_like))) < 3200, "a savefile's shape costs little more than its literal bytes")

    stream = pkg.qrle_encode(bytes(save_like))
    check(c_expand(work, stream[:-1], len(save_like)) is None, "qrle.c refuses the stream cut short by a byte")
    check(c_expand(work, stream, len(save_like) - 1) is None, "and one that expands past its size")
    try:
        pkg.qrle_decode(stream, len(save_like) + 1)
        check(False, "pkg.py's decoder refuses one that expands short of its size")
    except pkg.LibraryError:
        check(True, "")


# ---------------------------------------------------------------- a client checkout

PUBLISH = """
$ExpectedMods = @('NPEA00386', 'NPEA00423', 'libs')
$ShippedManifest = 'shipped.txt'
"""

RACK = bytes((i * 37 + 1) & 0xFF for i in range(534))
CAVE = bytes(40) + bytes(range(52))


def make_client(root):
    """A client checkout with every case the client's rules tell apart."""
    write(os.path.join(root, "publish.ps1"), PUBLISH)
    mods = os.path.join(root, "mods")
    # A mod with a .bin named twice in different case: uploaded once, in first appearance order.
    write(os.path.join(mods, "NPEA00386", "barlow", "patch.txt"),
          "#- name: Barlow trainer\r\n#- version: 1.0\r\n0x00123456: 0x60000000\r\n"
          "0x00400000: rack.bin\r\n0x00500000: RACK.BIN\r\n0x00600000: cave/igt.bin\r\n")
    write(os.path.join(mods, "NPEA00386", "barlow", "rack.bin"), RACK)
    write(os.path.join(mods, "NPEA00386", "barlow", "cave", "igt.bin"), CAVE)
    write(os.path.join(mods, "NPEA00386", "barlow", "src.zip"), b"PK sources the console never sees")
    write(os.path.join(mods, "NPEA00386", "barlow", "notes.txt"), "how it was made")
    # A Lua mod (publish.ps1's Remove-LuaMods drops it), a hidden one, a folder that is no mod.
    write(os.path.join(mods, "NPEA00386", "luamod", "patch.txt"), "#- name: lua\nautomation: run.lua\n")
    write(os.path.join(mods, "NPEA00386", "hidden", "patch.txt"), "#- name: hidden\n#- visible: false\n0x1: 0x2\n")
    write(os.path.join(mods, "NPEA00386", "notamod", "readme.txt"), "no patch.txt here")
    # A mod whose .bin is missing: the client uploads patch.txt alone, and hashes it alone.
    write(os.path.join(mods, "NPEA00423", "il-igt", "patch.txt"), "0x00200000: gone.bin\n0x00200004: 0x00000001\n")
    # The old client's Lua helpers, and a title publish.ps1 does not ship.
    write(os.path.join(mods, "libs", "NPEA00385", "Game.lua"), "-- lua")
    write(os.path.join(mods, "NPEA00387", "coolcam", "patch.txt"), "0x1: 0x2\n")

    saves = os.path.join(root, "premade-savefiles")
    a = bytes(720896)
    a = b"RAC1" + a[4:]
    b = bytearray(2 << 20)
    b[100:200] = bytes(range(100))
    write(os.path.join(saves, "NPEA00385", "NG+.sav"), a)
    write(os.path.join(saves, "NPEA00386", "NG+ & NG+ no IMG.sav"), bytes(b))
    write(os.path.join(saves, "NPEA00386", "notes.txt"), "not a save")
    write(os.path.join(saves, "titles.txt"),
          "# comment\n\nRC2ILYOOO        NPEA00386\nBCES01503/rac1   NPEA00385\nBCES01503/rac2   NPEA00386\n"
          "BCES01503/rac3   NPEA00387\n")
    return {"rac1": a, "rac2": bytes(b)}


def test_client_rules(work):
    group("the library takes what the client would upload, by the client's rules")
    client = os.path.join(work, "client")
    saves = make_client(client)
    entries = pkg.library_entries(client)
    files = dict((p, b) for _k, _g, p, b in entries)
    want_mods = [
        "mods/NPEA00386/barlow/patch.txt", "mods/NPEA00386/barlow/rack.bin", "mods/NPEA00386/barlow/cave/igt.bin",
        "mods/NPEA00386/barlow/qwark.sum", "mods/NPEA00423/il-igt/patch.txt", "mods/NPEA00423/il-igt/qwark.sum",
    ]
    check([p for k, _g, p, _b in entries if k == pkg.LIB_MOD_FILE] == want_mods,
          "the mods: patch.txt, each .bin once in patch order, qwark.sum; no Lua, hidden, libs or unshipped title")
    patch = open(os.path.join(client, "mods", "NPEA00386", "barlow", "patch.txt"), "rb").read()
    check(files["mods/NPEA00386/barlow/qwark.sum"] == ("%08x" % (zlib.crc32(CAVE, zlib.crc32(RACK, zlib.crc32(patch)))
                                                                 & 0xFFFFFFFF)).encode(),
          "qwark.sum is the client's hash: patch.txt, then the .bin files in patch order")
    lone = open(os.path.join(client, "mods", "NPEA00423", "il-igt", "patch.txt"), "rb").read()
    check(files["mods/NPEA00423/il-igt/qwark.sum"] == ("%08x" % (zlib.crc32(lone) & 0xFFFFFFFF)).encode(),
          "a missing .bin is left out of the files and of the hash, as the client leaves it")
    check([g for k, g, _p, _b in entries if k == pkg.LIB_MOD_FILE] == [0, 0, 0, 0, 1, 1], "each mod's files under one number")

    want_saves = {
        "savefiles/NPEA00385/(premade savefiles)/NG+.sav": saves["rac1"],
        "savefiles/NPEA00386/(premade savefiles)/NG+ & NG+ no IMG.sav": saves["rac2"],
        "savefiles/RC2ILYOOO/(premade savefiles)/NG+ & NG+ no IMG.sav": saves["rac2"],
        "savefiles/BCES01503/(premade savefiles) rac1/NG+.sav": saves["rac1"],
        "savefiles/BCES01503/(premade savefiles) rac2/NG+ & NG+ no IMG.sav": saves["rac2"],
    }
    got_saves = dict((p, b) for k, _g, p, b in entries if k == pkg.LIB_SAVE)
    check(got_saves == want_saves, "the savefiles: each title's own, and titles.txt's aliases in their categories; "
                                   "no line for a folder with nothing in it, nothing that is not a .sav")

    blob = pkg.make_library(entries)
    check(len(blob) < 12000, "the 7 MB of savefiles come to a few KB, each alias sharing its folder's bytes")
    check(pkg.read_library(blob) == entries, "read back by the installer's rules, it is the same files")

    group("pkg.py check finds a library that is not the client's")
    problems = []
    pkg.check_library(problems, blob, client)
    check(problems == [], "the library made from the checkout passes")
    write(os.path.join(client, "premade-savefiles", "NPEA00385", "NG+.sav"), b"RAC2" + saves["rac1"][4:])
    problems = []
    pkg.check_library(problems, blob, client)
    check(len(problems) == 1 and "BCES01503/(premade savefiles) rac1/NG+.sav" in problems[0] and "different" in problems[0],
          "a savefile changed in the checkout since is named")
    write(os.path.join(client, "premade-savefiles", "NPEA00385", "NG+.sav"), saves["rac1"])

    damaged = bytearray(blob)
    damaged[-1] ^= 0x01
    problems = []
    pkg.check_library(problems, bytes(damaged), client)
    check(len(problems) == 1, "a library with its last byte changed is refused")

    for name, path in (("a path with ..", "mods/NPEA00386/../x/patch.txt"), ("a hidden name", "mods/NPEA00386/barlow/.x"),
                       ("a savefile outside a category", "savefiles/NPEA00385/NG+.sav")):
        kind = pkg.LIB_SAVE if path.startswith("savefiles") else pkg.LIB_MOD_FILE
        try:
            pkg.read_library(pkg.make_library([(kind, 0, path, b"x")]))
            check(False, "%s is refused" % name)
        except pkg.LibraryError:
            check(True, "")
    return client


# ---------------------------------------------------------------- stage

def fake_package(work):
    build = pkg.qwark_build()
    sprx = os.path.join(work, "qwark.sprx")
    pak = os.path.join(work, "ui.pak")
    write(sprx, b"SCE\0" + bytes(range(200)))
    write(pak, b"QPAK" + b"\0\0\0\2" + struct.pack(">I", build) + bytes(100))
    return sprx, pak


def test_stage(work, client):
    group("pkg.py stage puts the library in the package's USRDIR")
    sprx, pak = fake_package(work)
    out = os.path.join(work, "stage")
    pkg.cmd_stage(types.SimpleNamespace(sprx=sprx, pak=pak, out=out, games=[], client=client))
    lib = os.path.join(out, "pkg", *pkg.LIBRARY_FILE.split("/"))
    check(os.path.isfile(lib) and open(lib, "rb").read() == pkg.make_library(pkg.library_entries(client)),
          "USRDIR/library.bin is the library of the checkout")
    write(os.path.join(out, "pkg", "USRDIR", "EBOOT.BIN"), b"SCE\0eboot")
    items = [n for n, _f, _b in pkg.items_of(os.path.join(out, "pkg"))]
    check(items[-1] == pkg.LIBRARY_FILE, "and the package's items carry it, last")

    pkg.cmd_stage(types.SimpleNamespace(sprx=sprx, pak=pak, out=out, games=[], client=None))
    check(not os.path.exists(lib), "without --client there is none")
    return sprx, pak


# ---------------------------------------------------------------- the console's own code

def tree(root):
    out = {}
    for dirpath, _dirs, names in os.walk(root):
        for n in names:
            p = os.path.join(dirpath, n)
            out[os.path.relpath(p, root).replace(os.sep, "/")] = open(p, "rb").read()
    return out


def install(root):
    p = subprocess.run([EXE, "--install", root], capture_output=True, text=True, timeout=TIMEOUT)
    first, _, msg = p.stdout.partition("\n")
    words = first.split()
    return dict(zip(words[0::2], [int(w) for w in words[1::2]])), msg


def test_install(work, client, sprx, pak):
    group("a library pkg.py made, installed by install.c onto a fake console")
    root = os.path.join(work, "console")
    usrdir = os.path.join(root, "dev_hdd0", "game", pkg.TITLE_ID, "USRDIR")
    entries = pkg.library_entries(client)
    write(os.path.join(usrdir, "qwark.sprx"), open(sprx, "rb").read())
    write(os.path.join(usrdir, "ui.pak"), open(pak, "rb").read())
    write(os.path.join(usrdir, "library.bin"), pkg.make_library(entries))
    os.makedirs(os.path.join(root, "dev_hdd0", "tmp"), exist_ok=True)

    r, msg = install(root)
    check(r.get("ok") == 1 and r.get("mods") == 2 and r.get("saves") == 5 and r.get("mods_failed") == 0 and
          r.get("saves_failed") == 0, "qwark, both mods and all five savefiles installed: %s" % r)
    qwark = os.path.join(root, "dev_hdd0", "qwark")
    on_console = tree(qwark)
    check(all(on_console.get(p) == b for _k, _g, p, b in entries),
          "every file of the library is on the console at its path under /dev_hdd0/qwark, byte for byte")
    check(all(on_console.get(p + ".sum") == ("%08x" % (zlib.crc32(b) & 0xFFFFFFFF)).encode()
              for k, _g, p, b in entries if k == pkg.LIB_SAVE), "each savefile with the client's .sum beside it")
    check("Added 2 mods and 5 premade savefiles." in msg, "the message counts them")

    group("installed again: everything is skipped, and the files are untouched")
    mod = os.path.join(qwark, "mods", "NPEA00386", "barlow", "patch.txt")
    write(mod, "my own edit")
    before = tree(qwark)
    r, msg = install(root)
    after = tree(qwark)
    check(r.get("mods_skipped") == 2 and r.get("saves_skipped") == 5 and r.get("mods") == 0 and r.get("saves") == 0,
          "two mods and five savefiles skipped: %s" % r)
    check(all(after.get(p) == b for p, b in before.items() if p.startswith(("mods/", "savefiles/"))) and
          set(p for p in after if p.startswith(("mods/", "savefiles/"))) ==
          set(p for p in before if p.startswith(("mods/", "savefiles/"))),
          "every mod and savefile file as it was, the user's edit included, and none added")
    check("Skipped 2 mods and 5 savefiles that were already installed." in msg, "the message says so")


def main():
    if not os.path.isfile(EXE):
        raise SystemExit("test_pkgdata.py: no %s; installer/test/run.sh builds it" % EXE)
    work = tempfile.mkdtemp(prefix="qwark-pkgdata-")
    try:
        test_qrle(work)
        client = test_client_rules(work)
        sprx, pak = test_stage(work, client)
        test_install(work, client, sprx, pak)
    finally:
        shutil.rmtree(work, ignore_errors=True)
        shutil.rmtree(os.path.join(INSTALLER, "build", "check"), ignore_errors=True)
    print("pkgdata: %d passed, %d failed" % (passed, failed))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
