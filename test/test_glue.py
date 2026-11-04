"""Tools of Destruction's console menu (revision 1.29): the pause
menu's Lua glue and the helper's look, run by a real Lua (docs/tod-menu.md 11).

The glue is Lua 5.0 for the game's own VM, and nothing in qwark's tests runs
Lua: this does, with a Lua found on the PC (a Lua 5.1 or 5.4 DLL, through
ctypes), against the stand-in pause menu of test/glue_harness.lua. It runs
both the source (src/games/luahelper/lh_glue.lua) and the bytes qwark sends
(lh_tod_glue in src/games/luahelper_bins.c, after build.sh's lua_min), so
the minified glue is held to the same cases, and the look (lh_tod_diag).
Revision 1.29: the galactic map's glue (lh_gmap.lua, lh_tod_gmap) the same
way, against the stand-in map of test/gmap_harness.lua. Revision 1.29: a console sends the three out of ui.pak, so "as sent" is the
pak's (pak.py --tod, as a test build and a release pack it), which must be the bytes
luahelper_bins.c gives the emulator builds; that is checked first, with or
without a Lua.

The DLL is QWARK_TEST_LUA when that is set, else the first of a few known
places; with none it says SKIP and passes, as the game's Lua cannot be had
here and these cases are what the console list in docs/tod-menu.md checks
first. Nothing is installed or downloaded.
"""
import ctypes
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

CANDIDATES = [
    os.path.expanduser(r"~\Documents\RaCMAN\x64\lua54.dll"),
    os.path.expanduser(r"~\Documents\RaCMAN\lua54.dll"),
]


def find_lua():
    env = os.environ.get("QWARK_TEST_LUA")
    if env:
        return env
    for c in CANDIDATES:
        if os.path.isfile(c):
            return c
    return None


def bins_array(name):
    """The bytes of `static const u8 <name>[]` in luahelper_bins.c."""
    src = open(os.path.join(ROOT, "src", "games", "luahelper_bins.c"), encoding="ascii").read()
    m = re.search(r"static const u8 " + re.escape(name) + r"\[\] = \{(.*?)\};", src, re.S)
    if m is None:
        raise SystemExit("test_glue: no %s in luahelper_bins.c" % name)
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))


def pak_chunks():
    """
    Revision 1.29: the pause menu's glue, the look and the map's glue
    as a console sends them, out of the pak a test build and a release pack (pak.py --tod),
    read back by pak.py's own reader: {kind: bytes}.
    """
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "qwark_pak", os.path.join(ROOT, "src", "games", "menuhelper", "pak.py"))
    pak = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pak)
    data = pak.pak_bytes(pak.parse_table(), pak.parse_sf_table(), pak.qwark_build(), pak.parse_lh_table())
    lua = []
    pak.parse_pak(data, lua)
    return dict((r["kind"], r["bytes"]) for r in lua if r["kind"] != pak.KIND_LUAHELPER)


LUA_CFUNC = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)


class Lua:
    """Just enough of the C API of a Lua 5.1 or 5.4 DLL."""

    def __init__(self, path):
        self.dll = ctypes.CDLL(path)
        d = self.dll
        d.luaL_newstate.restype = ctypes.c_void_p
        d.luaL_openlibs.argtypes = [ctypes.c_void_p]
        d.lua_close.argtypes = [ctypes.c_void_p]
        d.lua_tolstring.restype = ctypes.c_char_p
        d.lua_tolstring.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
        d.lua_pushvalue.argtypes = [ctypes.c_void_p, ctypes.c_int]
        d.lua_setmetatable.argtypes = [ctypes.c_void_p, ctypes.c_int]
        d.lua_pushcclosure.argtypes = [ctypes.c_void_p, LUA_CFUNC, ctypes.c_int]
        self.v54 = hasattr(d, "lua_newuserdatauv")
        if self.v54:
            d.lua_newuserdatauv.restype = ctypes.c_void_p
            d.lua_newuserdatauv.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
            d.luaL_loadbufferx.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p,
                                           ctypes.c_char_p]
            d.lua_pcallk.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_void_p,
                                     ctypes.c_void_p]
            d.lua_setglobal.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        else:
            d.lua_newuserdata.restype = ctypes.c_void_p
            d.lua_newuserdata.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
            d.luaL_loadbuffer.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p]
            d.lua_pcall.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
            d.lua_setfield.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p]
        self.L = d.luaL_newstate()
        d.luaL_openlibs(self.L)
        # newud(mt): a userdata with that metatable, which plain Lua 5.4 cannot make.
        self._newud = LUA_CFUNC(self.newud)
        d.lua_pushcclosure(self.L, self._newud, 0)
        self.setglobal(b"newud")

    def newud(self, L):
        if self.v54:
            self.dll.lua_newuserdatauv(L, 8, 1)
        else:
            self.dll.lua_newuserdata(L, 8)
        self.dll.lua_pushvalue(L, 1)
        self.dll.lua_setmetatable(L, -2)
        return 1

    def setglobal(self, name):
        if self.v54:
            self.dll.lua_setglobal(self.L, name)
        else:
            self.dll.lua_setfield(self.L, -10002, name)

    def run(self, chunk, name):
        d = self.dll
        if self.v54:
            st = d.luaL_loadbufferx(self.L, chunk, len(chunk), name, None)
            if st == 0:
                st = d.lua_pcallk(self.L, 0, 0, 0, None, None)
        else:
            st = d.luaL_loadbuffer(self.L, chunk, len(chunk), name)
            if st == 0:
                st = d.lua_pcall(self.L, 0, 0, 0)
        if st != 0:
            msg = d.lua_tolstring(self.L, -1, None)
            return msg.decode("latin-1") if msg else "error %d" % st
        return None

    def close(self):
        self.dll.lua_close(self.L)


def lua_str(b):
    """A Lua long string literal of the bytes b."""
    level = 0
    while (b"]" + b"=" * level + b"]") in b:
        level += 1
    eq = b"=" * level
    return b"[" + eq + b"[\n" + b + b"]" + eq + b"]"


def main():
    sent = pak_chunks()
    if [sent.get(4), sent.get(5), sent.get(6)] != [bins_array("lh_tod_glue"), bins_array("lh_tod_diag"),
                                                     bins_array("lh_tod_gmap")]:
        print("FAIL test_glue: ui.pak's three chunks are not luahelper_bins.c's")
        return 1
    print("test_glue: ui.pak's glue, look and map glue are luahelper_bins.c's, byte for byte")
    path = find_lua()
    if path is None or not os.path.isfile(path):
        print("SKIP test_glue: no Lua DLL (set QWARK_TEST_LUA to a Lua 5.1 or 5.4 DLL)")
        return 0
    harness = open(os.path.join(HERE, "glue_harness.lua"), "rb").read()
    glue_src = open(os.path.join(ROOT, "src", "games", "luahelper", "lh_glue.lua"), "rb").read().replace(b"\r", b"")
    diag_src = open(os.path.join(ROOT, "src", "games", "luahelper", "lh_diag.lua"), "rb").read().replace(b"\r", b"")
    runs = [("source", glue_src, diag_src), ("as sent", sent[4], sent[5])]
    print("test_glue: %s" % path)
    failed = 0
    for name, glue, diag in runs:
        lua = Lua(path)
        chunk = (b"GLUE = " + lua_str(glue) + b"\nDIAG = " + lua_str(diag) + b"\nNAME = \"" +
                 name.encode() + b"\"\n" + harness)
        err = lua.run(chunk, b"=glue_harness")
        lua.close()
        if err is not None:
            print("FAIL test_glue (%s): %s" % (name, err))
            failed += 1
    # Revision 1.29, the galactic map (docs/tod-menu.md 17): the map's glue against the stand-in map.
    gmap_harness = open(os.path.join(HERE, "gmap_harness.lua"), "rb").read()
    gmap_src = open(os.path.join(ROOT, "src", "games", "luahelper", "lh_gmap.lua"), "rb").read().replace(b"\r", b"")
    for name, gmap in (("map, source", gmap_src), ("map, as sent", sent[6])):
        lua = Lua(path)
        chunk = b"GMAP = " + lua_str(gmap) + b"\nNAME = \"" + name.encode() + b"\"\n" + gmap_harness
        err = lua.run(chunk, b"=gmap_harness")
        lua.close()
        if err is not None:
            print("FAIL test_glue (%s): %s" % (name, err))
            failed += 1
    print("test_glue: %s" % ("all passed" if failed == 0 else "%d run(s) failed" % failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
