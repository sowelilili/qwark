#!/usr/bin/env python3
"""
End-to-end smoke test for qwark-host.

Starts the simulator, drives its fake console over stdin and talks the real wire
protocol over TCP 9673 and UDP telemetry, exactly as docs/PROTOCOL.md describes.

Every step prints PASS or FAIL. The exit code is 0 only when everything passed.

Usage:  python test/smoke.py [path-to-qwark-host.exe]
"""

import os
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

PORT = 9673
HOST = "127.0.0.1"

# ---------------------------------------------------------------- protocol

# QWARK_BUILD in src/core/proto.h: the module build number, bumped whenever the
# feature tables or any user-visible behaviour change.
QWARK_BUILD = 3

OP_HELLO = 0x0001
OP_PREVIOUS_LIST = 0x0004
OP_PREVIOUS_REAPPLY = 0x0005
OP_PREVIOUS_DISMISS = 0x0006
OP_SUBSCRIBE = 0x0010
OP_GET_STATE = 0x0012
OP_DESCRIBE = 0x0020
OP_FEATURE_SET = 0x0021
OP_FEATURE_TRIGGER = 0x0022
OP_FEATURE_SET_AUTO = 0x0023
OP_FEATURE_OPTIONS = 0x0024
OP_MEM_READ = 0x0030
OP_MEM_WRITE = 0x0031
OP_WATCH_ADD = 0x0032
OP_WATCH_LIST = 0x0034
OP_FREEZE_ADD = 0x0035
OP_FREEZE_LIST = 0x0037
OP_POS_SELECT = 0x0040
OP_POS_SAVE = 0x0041
OP_POS_LIST = 0x0043
OP_PLANET_LIST = 0x0045
OP_PLANET_LOAD = 0x0047
OP_MOBY_TABLE = 0x0049
OP_UNLOCK_LIST = 0x0050
OP_UNLOCK_SET = 0x0051
OP_LEVELFLAGS_GET = 0x0053
OP_LEVELFLAGS_RESET = 0x0054
OP_LEVELFLAGS_SET = 0x0055
OP_MOD_LIST = 0x0060
OP_COMBO_SET = 0x0080
OP_COMBO_LIST = 0x0081
OP_CONFIG_RELOAD = 0x0090
OP_UNKNOWN = 0x7FFF

ST_OK = 0
ST_UNSUPPORTED = 2
ST_BAD_ARG = 3
ST_UNKNOWN_OP = 6

SESSION_XMB, SESSION_BOOTING, SESSION_INGAME, SESSION_QUITTING = 0, 1, 2, 3

# Protocol 1.1: sixteen readouts, so SessionInfo is 164 bytes.
SESSION_INFO_SIZE = 164
FEATURE_WIRE_SIZE = 48
UNLOCK_WIRE_SIZE = 44
UNLOCK_FIELD_WIRE_SIZE = 16

# UnlockFieldDesc.kind
UNLOCK_KIND_FLAG = 0
UNLOCK_KIND_NUMBER = 1

FEATURE_TOGGLE, FEATURE_ACTION, FEATURE_VALUE, FEATURE_ENUM, FEATURE_COLOR = 0, 1, 2, 3, 4

# Feature.flags
FEATURE_FLAG_AUTO = 0x01
FEATURE_FLAG_WRITES_CODE = 0x02
FEATURE_FLAG_SAVE_ASIDE = 0x04
FEATURE_FLAG_LOAD_ASIDE = 0x08
# Protocol 1.3: the toggle's state is read back out of game memory.
FEATURE_FLAG_LIVE = 0x10

COMBO_SAVE_POSITION = 0

PLANET_FLAG_RESET_LEVELFLAGS = 0x01
PLANET_FLAG_RESET_BOLTS = 0x02

# RaC1 addresses, only so the test can poke something recognisable.
RAC1_COORDS = 0x969D60
RAC1_BOLTS = 0x969CA0
RAC1_LEVEL_FLAGS = 0xA0CA84
RAC1_MISC_FLAGS = 0xA0CD1C
RAC1_GOLD_BOLTS = 0xA0CA34
RAC1_LOAD_PLANET = 0xA10700
RAC1_DEBUG_MODE = 0x95C5D4
RAC1_JANKPOT_BOLTS = 0xA0FD18
RAC1_GOODIES_MENU = 0x969CD3

# RaC1 feature ids, from src/games/rac1.h.
F_INFINITE_AMMO = 1
F_BOLTS = 5
F_GOODIES = 6
F_JANK_BOLTS = 21
F_DBG_RATCHET = 24
F_DBG_MOBYS = 25
F_DBG_PARTICLES = 26
F_DBG_CAMERA = 27

# RaC1 readout slots, from src/games/rac1.h.
RO_BOLTS = 0
RO_JANK_BOLTS = 3
RO_CAMERA = 7
RO_GOODIES = 11

# ------------------------------------------------------------- test harness

passed = 0
failed = 0


def check(ok, what, detail=""):
    global passed, failed
    if ok:
        passed += 1
        print("PASS  %s" % what)
    else:
        failed += 1
        print("FAIL  %s%s" % (what, (" -- " + str(detail)) if detail else ""))
    return ok


class Client:
    def __init__(self, sock):
        self.sock = sock
        self.seq = 0

    def call(self, op, payload=b""):
        self.seq = (self.seq + 1) & 0xFFFF
        seq = self.seq
        self.sock.sendall(struct.pack(">IHH", len(payload), seq, op) + payload)

        header = self.recv_exact(8)
        length, rseq, status = struct.unpack(">IHH", header)
        body = self.recv_exact(length) if length else b""

        if rseq != seq:
            raise RuntimeError("reply seq %d for request %d" % (rseq, seq))
        return status, body

    def recv_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise RuntimeError("connection closed")
            buf += chunk
        return buf


class Sim:
    def __init__(self, exe):
        self.proc = subprocess.Popen(
            [exe],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=ROOT,
            text=True,
            bufsize=1,
        )
        self.lines = []
        self._drain(self.proc.stderr)
        self._drain(self.proc.stdout, self.lines)

    def _drain(self, stream, sink=None):
        def run():
            for line in stream:
                if sink is not None:
                    sink.append(line.rstrip())
        t = threading.Thread(target=run, daemon=True)
        t.start()

    def send(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def stop(self):
        try:
            self.send("exit")
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()


# ------------------------------------------------------------- decoding

def parse_session_info(b):
    if len(b) < SESSION_INFO_SIZE:
        raise ValueError("session info is %d bytes" % len(b))

    info = {}
    info["protocol"] = b[0]
    info["build"] = b[1]
    info["state"] = b[2]
    info["game"] = b[3]
    info["generation"] = struct.unpack(">I", b[4:8])[0]
    info["tick"] = struct.unpack(">I", b[8:12])[0]
    info["title"] = b[12:24].split(b"\0")[0].decode("ascii", "replace")
    info["flags"] = b[24]
    info["slot"] = b[25]
    info["planet_sel"] = b[26]
    info["planet_flags"] = b[27]
    info["planet"] = b[28]
    info["pos"] = struct.unpack(">3f", b[32:44])
    info["pad_mask"] = struct.unpack(">I", b[44:48])[0]
    info["analog"] = struct.unpack(">4f", b[48:64])
    info["readout"] = struct.unpack(">16I", b[64:128])
    info["toggle_state"] = struct.unpack(">Q", b[128:136])[0]
    info["toggle_auto"] = struct.unpack(">Q", b[136:144])[0]
    info["freeze_active"] = struct.unpack(">Q", b[144:152])[0]
    info["mod_loaded"] = struct.unpack(">I", b[152:156])[0]
    info["mod_auto"] = struct.unpack(">I", b[156:160])[0]
    info["mod_previous"] = struct.unpack(">I", b[160:164])[0]
    return info


def parse_telemetry(b):
    if b[:4] != b"QWRK":
        raise ValueError("bad magic %r" % b[:4])

    info = parse_session_info(b[4:4 + SESSION_INFO_SIZE])
    off = 4 + SESSION_INFO_SIZE
    nwatch = b[off]
    off += 1

    watches = []
    for _ in range(nwatch):
        wid, size, valid, _pad = b[off], b[off + 1], b[off + 2], b[off + 3]
        value = struct.unpack(">Q", b[off + 4:off + 12])[0]
        watches.append({"id": wid, "size": size, "valid": valid, "value": value})
        off += 12

    return info, watches, off


def parse_describe(b):
    off = 0
    game = b[off]; off += 1
    ngroups = b[off]; off += 1
    groups = []
    for _ in range(ngroups):
        groups.append(b[off:off + 24].split(b"\0")[0].decode("ascii", "replace"))
        off += 24
    nreadouts = b[off]; off += 1
    readouts = []
    for _ in range(nreadouts):
        readouts.append(b[off:off + 24].split(b"\0")[0].decode("ascii", "replace"))
        off += 24
    nfeatures = b[off]; off += 1
    features = []
    for _ in range(nfeatures):
        row = b[off:off + FEATURE_WIRE_SIZE]
        features.append({
            "id": row[0],
            "kind": row[1],
            "group": row[2],
            "aux": row[3],
            "flags": row[4],
            "readout": row[5],
            "min": struct.unpack(">I", row[8:12])[0],
            "max": struct.unpack(">I", row[12:16])[0],
            "label": row[16:48].split(b"\0")[0].decode("ascii", "replace"),
        })
        off += FEATURE_WIRE_SIZE
    return game, groups, readouts, features, off


def parse_unlocks(b):
    """UNLOCK_LIST, protocol 1.3: categories, four field descriptors, then rows."""
    off = 0
    ncat = b[off]; off += 1
    cats = []
    for _ in range(ncat):
        cats.append(b[off:off + 24].split(b"\0")[0].decode("ascii", "replace"))
        off += 24
    fields = []
    for _ in range(4):
        row = b[off:off + UNLOCK_FIELD_WIRE_SIZE]
        fields.append({
            "name": row[:12].split(b"\0")[0].decode("ascii", "replace"),
            "kind": row[12],
            "max": row[13],
            "reserved": row[14:16],
        })
        off += UNLOCK_FIELD_WIRE_SIZE
    n = b[off]; off += 1
    rows = []
    for _ in range(n):
        row = b[off:off + UNLOCK_WIRE_SIZE]
        rows.append({
            "id": row[0],
            "category": row[1],
            "fields": row[2],
            "values": struct.unpack(">4I", row[4:20]),
            "name": row[20:44].split(b"\0")[0].decode("ascii", "replace"),
        })
        off += UNLOCK_WIRE_SIZE
    return cats, fields, rows, off


# --------------------------------------------------------------- helpers

def fresh_state(client, settle=0.2):
    """Telemetry is published at 30 Hz, so let a frame go by before reading."""
    time.sleep(settle)
    status, body = client.call(OP_GET_STATE)
    if status != ST_OK:
        return None
    info, _watches, _n = parse_telemetry(body)
    return info


def wait_state(client, want, timeout=6.0, forbid=None):
    """Polls GET_STATE until the session reaches `want`. Returns (ok, info)."""
    deadline = time.time() + timeout
    info = None
    while time.time() < deadline:
        status, body = client.call(OP_GET_STATE)
        if status == ST_OK:
            info, _watches, _n = parse_telemetry(body)
            if forbid is not None and info["state"] == forbid:
                return False, info
            if info["state"] == want:
                return True, info
        time.sleep(0.05)
    return False, info


def connect(timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.create_connection((HOST, PORT), timeout=5)
            s.settimeout(10)
            return s
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("could not connect to qwark-host on %d" % PORT)


# ------------------------------------------- the other three games, end to end

def mem_write(c, addr, data):
    status, _ = c.call(OP_MEM_WRITE, struct.pack(">I", addr) + data)
    return status


def mem_read_u32(c, addr):
    status, body = c.call(OP_MEM_READ, struct.pack(">II", addr, 4))
    if status != ST_OK or len(body) != 4:
        return None
    return struct.unpack(">I", body)[0]


# One row per game: what to boot, what DESCRIBE must say, and one of each kind of
# request, so every game gets the same end-to-end pass the RaC1 steps give.
OTHER_GAMES = [
    {
        "title": "NPEA00386",
        "name": "RaC2",
        "game": 2,
        "features": 37,
        "readouts": 9,
        "planets": 27,
        "planet0": "Aranos",
        "unlocks": 44,
        "categories": 3,
        "unlock0": "Lancer",
        # RC2Unlocks.cs is one owned byte per row and nothing else.
        "fields": [("Owned", UNLOCK_KIND_FLAG, 0),
                   ("", UNLOCK_KIND_FLAG, 0),
                   ("", UNLOCK_KIND_FLAG, 0),
                   ("", UNLOCK_KIND_FLAG, 0)],
        "levelflags": 0x10,
        "coords": 0x147F260,
        # Bolts, a VALUE with readout 0.
        "value_id": 7,
        "value_addr": 0x1329A90,
        # "Reset platinum bolts", an ACTION with no helper gate.
        "action_id": 24,
        "action_addr": 0x1562540,
        "action_expect": 0,
        "planet": 5,
        # The classic two-word request: 00000001 000000XX.
        "load_addr": 0x156B050,
        "load_expect": [(0x156B050, 1), (0x156B054, 5)],
    },
    {
        "title": "NPEA00387",
        "name": "RaC3",
        "game": 3,
        "features": 32,
        # Retired, never renumbered: 4, 17, 28, 29 and 30.
        "retired": [4, 17, 28, 29, 30],
        "readouts": 12,
        "planets": 37,
        "planet0": "(none)",
        "unlocks": 41,
        "categories": 3,
        "unlock0": "Bomb Glove",
        # Slot 1 is UYA's weapon version, not a gold flag, and slot 2 is its XP.
        "fields": [("Owned", UNLOCK_KIND_FLAG, 0),
                   ("Level", UNLOCK_KIND_NUMBER, 8),
                   ("XP", UNLOCK_KIND_NUMBER, 0),
                   ("Ammo", UNLOCK_KIND_NUMBER, 0)],
        # The Suck Cannon carries no ammo the game counts.
        "no_field": [("Suck Cannon", 3)],
        "levelflags": 0x10,
        "coords": 0xDA2870,
        "value_id": 7,
        "value_addr": 0xC1E4DC,
        # "Reset all titanium bolts".
        "action_id": 25,
        "action_addr": 0xECE53D,
        "action_expect": 0,
        "planet": 5,
        "load_addr": 0xEE9310,
        "load_expect": [(0xEE9310, 1), (0xEE9314, 5), (0x134EBD4, 3)],
    },
    {
        "title": "NPEA00423",
        "name": "Deadlocked",
        "game": 4,
        "features": 15,
        "readouts": 8,
        "planets": 16,
        "planet0": "(unused)",
        "unlocks": 16,
        "categories": 1,
        "unlock0": "Pistol Flux LX",
        # A bot upgrade is one owned byte; Deadlocked's weapons are not listed.
        "fields": [("Owned", UNLOCK_KIND_FLAG, 0),
                   ("", UNLOCK_KIND_FLAG, 0),
                   ("", UNLOCK_KIND_FLAG, 0),
                   ("", UNLOCK_KIND_FLAG, 0)],
        # Deadlocked has never had a level-flag region.
        "levelflags": None,
        "coords": 0x10D44D0,
        "value_id": 7,
        "value_addr": 0x9C32E8,
        # "Act tune bosses" writes 20 into five one-byte tuning slots.
        "action_id": 12,
        "action_addr": 0xA947D3,
        "action_expect": 20,
        "planet": 4,
        # Deadlocked writes the planet then a 1 into a second word.
        "load_addr": 0xB36DD0,
        "load_expect": [(0xB36DD0, 4), (0xB36DCC, 1)],
    },
]


def exercise_game(c, sim, spec):
    name = spec["name"]

    sim.send("quit")
    ok, _ = wait_state(c, SESSION_XMB)
    check(ok, "%s: the previous game quit" % name)

    sim.send("boot %s" % spec["title"])
    ok, info = wait_state(c, SESSION_INGAME)
    check(ok, "%s: %s reaches INGAME" % (name, spec["title"]), info)
    if not ok:
        return
    check(info["game"] == spec["game"],
          "%s: the game byte is right" % name, info["game"])

    # ------------------------------------------------------------ describe
    status, body = c.call(OP_DESCRIBE)
    if check(status == ST_OK, "%s: DESCRIBE answers OK" % name, status):
        game, groups, readouts, features, consumed = parse_describe(body)
        by_id = {f["id"]: f for f in features}
        check(consumed == len(body), "%s: DESCRIBE parses exactly" % name)
        check(game == spec["game"], "%s: DESCRIBE names the game" % name, game)
        check(len(features) == spec["features"],
              "%s: the feature count matches" % name, len(features))
        if spec.get("retired"):
            still_there = [i for i in spec["retired"] if i in by_id]
            check(not still_there,
                  "%s: no retired feature id is described" % name, still_there)
        check(len(readouts) == spec["readouts"],
              "%s: the readout count matches" % name, len(readouts))
        check(readouts and readouts[0] == "Bolts",
              "%s: readout 0 is Bolts" % name, readouts)
        check(len(groups) <= 16 and len(readouts) <= 16 and len(features) <= 64,
              "%s: DESCRIBE stays inside its caps" % name)
        check(all(f["readout"] == 0xFF for f in features
                  if f["kind"] in (FEATURE_TOGGLE, FEATURE_ACTION)),
              "%s: every TOGGLE and ACTION names readout 0xFF" % name)

        # Protocol 1.2: exactly one of each savefile flag, both on ACTIONs.
        save_aside = [f for f in features if f["flags"] & 0x04]
        load_aside = [f for f in features if f["flags"] & 0x08]
        check(len(save_aside) == 1 and save_aside[0]["kind"] == FEATURE_ACTION,
              "%s: one SAVE_ASIDE action" % name, save_aside)
        check(len(load_aside) == 1 and load_aside[0]["kind"] == FEATURE_ACTION,
              "%s: one LOAD_ASIDE action" % name, load_aside)

        # Every ENUM must answer FEATURE_OPTIONS with the count it advertised.
        for f in features:
            if f["kind"] != FEATURE_ENUM:
                continue
            status, body = c.call(OP_FEATURE_OPTIONS, bytes([f["id"]]))
            check(status == ST_OK and body and body[0] == f["aux"],
                  "%s: the %s ENUM lists %d options" % (name, f["label"], f["aux"]),
                  (status, body[0] if body else None))

    # -------------------------------------------------------- planet list
    status, body = c.call(OP_PLANET_LIST)
    if check(status == ST_OK, "%s: PLANET_LIST answers OK" % name, status):
        count = body[0]
        first = body[1:25].split(b"\0")[0].decode("ascii", "replace")
        check(count == spec["planets"],
              "%s: the planet count matches" % name, count)
        check(first == spec["planet0"],
              "%s: index 0 is the game's own planet 0" % name, first)

    # -------------------------------------------------------- unlock list
    status, body = c.call(OP_UNLOCK_LIST)
    if check(status == ST_OK, "%s: UNLOCK_LIST answers OK" % name, status):
        cats, fields, rows, consumed = parse_unlocks(body)
        check(consumed == len(body), "%s: UNLOCK_LIST parses exactly" % name)
        check(len(rows) == spec["unlocks"],
              "%s: the unlock count matches" % name, len(rows))
        check(len(cats) == spec["categories"],
              "%s: the category count matches" % name, cats)
        check(rows and rows[0]["name"] == spec["unlock0"],
              "%s: row 0 is the expected item" % name,
              rows[0]["name"] if rows else None)

        # Protocol 1.3: four field descriptors, always four, naming the slots.
        check(len(fields) == 4, "%s: four field descriptors" % name, len(fields))
        check(all(f["reserved"] == b"\0\0" for f in fields),
              "%s: their reserved bytes are zero" % name)
        check(fields[0]["name"] == "Owned" and
              fields[0]["kind"] == UNLOCK_KIND_FLAG,
              "%s: slot 0 is the Owned checkbox" % name, fields[0])
        for slot, want in enumerate(spec["fields"]):
            got = (fields[slot]["name"], fields[slot]["kind"], fields[slot]["max"])
            check(got == want,
                  "%s: slot %d is %r" % (name, slot, want), got)

        # A slot the game leaves unnamed must be declared by no row at all.
        for slot in range(4):
            if fields[slot]["name"]:
                continue
            check(all((r["fields"] & (1 << slot)) == 0 for r in rows),
                  "%s: no row declares the unnamed slot %d" % (name, slot))

        # Rows that must not offer a slot the rest of their category does, and
        # whose UNLOCK_SET on it has to be refused rather than quietly written.
        for row_name, slot in spec.get("no_field", []):
            row = next((r for r in rows if r["name"] == row_name), None)
            if not check(row is not None,
                         "%s: %s is in the table" % (name, row_name)):
                continue
            check((row["fields"] & (1 << slot)) == 0,
                  "%s: %s declares no slot %d" % (name, row_name, slot),
                  hex(row["fields"]))
            status, _ = c.call(OP_UNLOCK_SET,
                               struct.pack(">BBHI", row["id"], slot, 0, 1))
            check(status == ST_UNSUPPORTED,
                  "%s: and UNLOCK_SET on it is UNSUPPORTED" % name, status)

        # And one round trip through UNLOCK_SET.
        status, _ = c.call(OP_UNLOCK_SET, struct.pack(">BBHI", 0, 0, 0, 1))
        check(status == ST_OK, "%s: UNLOCK_SET owned=1" % name, status)
        status, body = c.call(OP_UNLOCK_LIST)
        if status == ST_OK:
            _cats, _fields, rows, _n = parse_unlocks(body)
            check(rows and rows[0]["values"][0] == 1,
                  "%s: and it reads back live" % name,
                  rows[0]["values"] if rows else None)

    # -------------------------------------------------------- level flags
    # A game with no level-flag hooks answers UNSUPPORTED, which is what tells
    # the client to hide the panel; one that has them round-trips a byte.
    want = spec["levelflags"]
    status, body = c.call(OP_LEVELFLAGS_GET, bytes([5]))
    if want is None:
        check(status == ST_UNSUPPORTED and len(body) == 0,
              "%s: LEVELFLAGS_GET is UNSUPPORTED" % name, (status, len(body)))
    elif check(status == ST_OK, "%s: LEVELFLAGS_GET answers OK" % name, status):
        flen = struct.unpack(">H", body[0:2])[0]
        check(flen == want, "%s: it returns 0x%X bytes of flags" % (name, want),
              hex(flen))

        status, _ = c.call(OP_LEVELFLAGS_SET, struct.pack(">BBH", 5, 0xCC, 2))
        check(status == ST_OK, "%s: LEVELFLAGS_SET accepted" % name, status)
        status, body = c.call(OP_LEVELFLAGS_GET, bytes([5]))
        check(status == ST_OK and body[2 + 2] == 0xCC,
              "%s: and the byte reads back" % name, body[2:2 + 4])

        status, _ = c.call(OP_LEVELFLAGS_SET, struct.pack(">BBH", 5, 1, want))
        check(status == ST_BAD_ARG,
              "%s: one past the end is BAD_ARG" % name, status)

        status, _ = c.call(OP_LEVELFLAGS_RESET, bytes([5]))
        check(status == ST_OK, "%s: LEVELFLAGS_RESET accepted" % name, status)
        status, body = c.call(OP_LEVELFLAGS_GET, bytes([5]))
        check(status == ST_OK and set(body[2:]) == {0},
              "%s: and every flag byte is zero" % name, status)

    # -------------------------------------------------------- FEATURE_SET
    status, _ = c.call(OP_FEATURE_SET, struct.pack(">BI", spec["value_id"], 4242))
    if check(status == ST_OK, "%s: FEATURE_SET on the bolt count" % name, status):
        check(mem_read_u32(c, spec["value_addr"]) == 4242,
              "%s: and the word landed" % name)
        info = fresh_state(c)
        check(info and info["readout"][0] == 4242,
              "%s: readout 0 mirrors it" % name,
              info["readout"][0] if info else None)

    # ---------------------------------------------------- FEATURE_TRIGGER
    mem_write(c, spec["action_addr"], b"\xAA")
    status, _ = c.call(OP_FEATURE_TRIGGER, bytes([spec["action_id"]]))
    if check(status == ST_OK, "%s: FEATURE_TRIGGER runs" % name, status):
        status, body = c.call(OP_MEM_READ, struct.pack(">II", spec["action_addr"], 1))
        check(status == ST_OK and body[0] == spec["action_expect"],
              "%s: and the action wrote what it should" % name,
              body[0] if body else None)

    # ------------------------------------------------- positions, one slot
    c.call(OP_POS_SELECT, bytes([0]))
    # 10.0, 50.0, -10.0 as three big-endian floats.
    mem_write(c, spec["coords"], struct.pack(">3f", 10.0, 50.0, -10.0))
    status, _ = c.call(OP_POS_SAVE, bytes([0]))
    if check(status == ST_OK, "%s: POS_SAVE stores slot 0" % name, status):
        status, body = c.call(OP_POS_LIST)
        if check(status == ST_OK, "%s: POS_LIST answers OK" % name, status):
            nslots = body[1]
            filled = body[2]
            x, y, z = struct.unpack(">3f", body[6:18])
            check(nslots == 8, "%s: eight slots per planet" % name, nslots)
            check(filled == 1, "%s: slot 0 is filled" % name, filled)
            check(abs(x - 10.0) < 0.01 and abs(y - 50.0) < 0.01 and
                  abs(z + 10.0) < 0.01,
                  "%s: with the coordinates that were saved" % name, (x, y, z))

    # -------------------------------------------------------- planet load
    for addr, _want in spec["load_expect"]:
        mem_write(c, addr, struct.pack(">I", 0))
    status, _ = c.call(OP_PLANET_LOAD, bytes([spec["planet"], 0]))
    if check(status == ST_OK, "%s: PLANET_LOAD accepted" % name, status):
        for addr, want in spec["load_expect"]:
            check(mem_read_u32(c, addr) == want,
                  "%s: the load request word at 0x%X is %d" % (name, addr, want),
                  mem_read_u32(c, addr))


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "qwark-host.exe")
    if not os.path.exists(exe):
        print("FAIL  %s does not exist, run build-host.sh first" % exe)
        return 2

    # A clean fake HDD, with the mod fixtures installed.
    host_root = os.path.join(ROOT, "qwark-host-root")
    shutil.rmtree(host_root, ignore_errors=True)
    mods_dst = os.path.join(host_root, "dev_hdd0", "qwark", "mods")
    os.makedirs(mods_dst, exist_ok=True)
    fixtures = os.path.join(HERE, "fixtures", "mods")
    if os.path.isdir(fixtures):
        for name in os.listdir(fixtures):
            shutil.copytree(os.path.join(fixtures, name), os.path.join(mods_dst, name))

    sim = Sim(exe)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(("0.0.0.0", 0))
    udp_port = udp.getsockname()[1]
    udp.settimeout(2.0)

    try:
        sock = connect()
        c = Client(sock)

        # ---------------------------------------------------------- hello
        status, body = c.call(OP_HELLO, bytes([1]))
        info = parse_session_info(body) if status == ST_OK else None
        check(status == ST_OK and len(body) == SESSION_INFO_SIZE,
              "HELLO returns a 164-byte SessionInfo", (status, len(body)))
        check(info and info["protocol"] == 1, "protocol version is 1")
        check(info and info["build"] == QWARK_BUILD,
              "HELLO reports build %d" % QWARK_BUILD,
              info["build"] if info else None)

        # ------------------------------------------------------- subscribe
        status, _ = c.call(OP_SUBSCRIBE, struct.pack(">H", udp_port))
        check(status == ST_OK, "SUBSCRIBE accepted", status)

        # ------------------------------------------------------------ boot
        sim.send("boot NPEA00385")
        ok, info = wait_state(c, SESSION_INGAME)
        check(ok, "the session reaches INGAME after boot NPEA00385", info)
        check(info and info["title"] == "NPEA00385", "the title id is reported", info)
        check(info and info["game"] == 1, "the game byte says RaC1", info)
        gen_first = info["generation"] if info else 0

        # ------------------------------------------------------- telemetry
        # Packets from the BOOTING phase may still be queued, so keep reading
        # until one says INGAME.
        got_packet = None
        deadline = time.time() + 3.0
        while time.time() < deadline:
            try:
                data, _addr = udp.recvfrom(2048)
            except socket.timeout:
                break
            if data[:4] != b"QWRK":
                continue
            got_packet = data
            if data[4 + 2] == SESSION_INGAME:
                break
        check(got_packet is not None, "a UDP telemetry packet arrived")
        if got_packet:
            tinfo, twatches, consumed = parse_telemetry(got_packet)
            check(consumed == len(got_packet),
                  "the telemetry packet parses with the documented layout",
                  (consumed, len(got_packet)))
            check(tinfo["state"] == SESSION_INGAME, "telemetry says INGAME", tinfo["state"])
            check(len(got_packet) <= 937, "the packet is at most 937 bytes", len(got_packet))

        # -------------------------------------------------------- describe
        by_id = {}
        status, body = c.call(OP_DESCRIBE)
        if check(status == ST_OK, "DESCRIBE answers OK", status):
            game, groups, readouts, features, consumed = parse_describe(body)
            by_id = {f["id"]: f for f in features}
            check(consumed == len(body), "DESCRIBE parses exactly", (consumed, len(body)))
            check(game == 1, "DESCRIBE names RaC1")
            check(len(features) == 27, "twenty-seven features", len(features))
            check(9 not in by_id, "the retired id 9 is not described", sorted(by_id))
            check(8 in by_id and 10 in by_id,
                  "and the ids either side of it kept their numbers")
            check(len(groups) <= 16 and len(readouts) <= 16 and len(features) <= 64,
                  "DESCRIBE stays inside its caps", (len(groups), len(readouts)))
            labels = [f["label"] for f in features]
            check("Fast loads" in labels and "Infinite ammo" in labels,
                  "the expected feature labels are present", labels)
            check(readouts and readouts[0] == "Bolts", "readout 0 is Bolts", readouts)
            check("Savefile helper" in readouts and "Camera mode" in readouts,
                  "the 1.1 readouts are declared", readouts)

            # Protocol 1.1: the readout byte at offset 5.
            check(all(f["readout"] == 0xFF for f in features
                      if f["kind"] in (FEATURE_TOGGLE, FEATURE_ACTION)),
                  "every TOGGLE and ACTION names readout 0xFF")
            check(by_id.get(F_BOLTS, {}).get("readout") == RO_BOLTS,
                  "the bolts VALUE mirrors readout 0", by_id.get(F_BOLTS))
            cam = by_id.get(F_DBG_CAMERA, {})
            check(cam.get("kind") == FEATURE_ENUM and cam.get("aux") == 3
                  and cam.get("readout") == RO_CAMERA and cam.get("max") == 2,
                  "the camera ENUM carries its option count, readout and range", cam)

        # ------------------------------------------------- feature options
        status, body = c.call(OP_FEATURE_OPTIONS, bytes([F_DBG_CAMERA]))
        if check(status == ST_OK, "FEATURE_OPTIONS answers for the camera ENUM", status):
            count = body[0]
            names = [body[1 + i * 24:1 + i * 24 + 24].split(b"\0")[0].decode()
                     for i in range(count)]
            check(count == 3 and names[0] == "Normal"
                  and names[2] == "Freecam character",
                  "the three camera modes come back", names)

        status, _ = c.call(OP_FEATURE_OPTIONS, bytes([0]))
        check(status == ST_BAD_ARG,
              "asking a TOGGLE for options is BAD_ARG", status)

        # --------------------------------------------- a VALUE readout round trip
        c.call(OP_FEATURE_SET, struct.pack(">BI", F_DBG_CAMERA, 2))
        status, body = c.call(OP_MEM_READ, struct.pack(">II", RAC1_DEBUG_MODE, 4))
        check(body == struct.pack(">I", 2),
              "setting the camera ENUM wrote the mode word", body)

        status, _ = c.call(OP_FEATURE_SET, struct.pack(">BI", F_JANK_BOLTS, 596524))
        check(status == ST_OK, "FEATURE_SET on the jankpot bolts VALUE", status)
        status, body = c.call(OP_MEM_READ, struct.pack(">II", RAC1_JANKPOT_BOLTS, 4))
        check(body == struct.pack(">I", 596524), "it reached memory", body)

        # The jankpot and debug blocks are sampled every eighth tick.
        info = fresh_state(c, settle=0.4)
        check(info and info["readout"][RO_JANK_BOLTS] == 596524,
              "the readout mirrors the value back",
              info["readout"][RO_JANK_BOLTS] if info else None)
        check(info and info["readout"][RO_CAMERA] == 2,
              "and the camera readout follows the ENUM",
              info["readout"][RO_CAMERA] if info else None)
        c.call(OP_FEATURE_SET, struct.pack(">BI", F_DBG_CAMERA, 0))

        status, _ = c.call(OP_FEATURE_SET, struct.pack(">BI", F_DBG_CAMERA, 9))
        check(status == ST_BAD_ARG, "a value outside the ENUM range is BAD_ARG", status)

        # ------------------------------------------------------ planet list
        status, body = c.call(OP_PLANET_LIST)
        if check(status == ST_OK, "PLANET_LIST answers OK", status):
            count = body[0]
            names = [body[1 + i * 24:1 + i * 24 + 24].split(b"\0")[0].decode()
                     for i in range(count)]
            check(count == 19, "RaC1 has 19 planets", count)
            check(names[0] == "Veldin" and names[3] == "Kerwan",
                  "the planet names line up", names[:4])

        # ----------------------------------------------------------- unlocks
        status, body = c.call(OP_UNLOCK_LIST)
        if check(status == ST_OK, "UNLOCK_LIST answers OK", status):
            cats, fields, rows, consumed = parse_unlocks(body)
            check(consumed == len(body), "UNLOCK_LIST parses exactly",
                  (consumed, len(body)))
            check(cats == ["Weapons", "Gadgets", "Items"],
                  "the three categories come back", cats)
            check(len(rows) == 39, "the whole unlock table is there", len(rows))
            check(rows[0]["name"] == "Bomb Glove", "entry 0 is the Bomb Glove", rows[0])
            check(rows[0]["fields"] == 0x01 | 0x02 | 0x08,
                  "a weapon with ammo declares owned, gold and ammo",
                  hex(rows[0]["fields"]))
            check(rows[34]["fields"] == 0x01,
                  "an index-less item declares only owned", hex(rows[34]["fields"]))

            # Protocol 1.3: RaC1's four slots, named honestly. Gold really is a
            # RaC1 idea, so this is the one game that has a Gold checkbox.
            check([(f["name"], f["kind"], f["max"]) for f in fields] ==
                  [("Owned", UNLOCK_KIND_FLAG, 0),
                   ("Gold", UNLOCK_KIND_FLAG, 0),
                   ("", UNLOCK_KIND_FLAG, 0),
                   ("Ammo", UNLOCK_KIND_NUMBER, 0)],
                  "RaC1 names Owned, Gold, nothing and Ammo", fields)

        # Owning a weapon hands it its max ammo, as the old client did.
        c.call(OP_MEM_WRITE, struct.pack(">I", 0x96C0D4) + struct.pack(">I", 0))
        status, _ = c.call(OP_UNLOCK_SET, struct.pack(">BBHI", 0, 0, 0, 1))
        check(status == ST_OK, "UNLOCK_SET owned=1 on the Bomb Glove", status)
        status, body = c.call(OP_MEM_READ, struct.pack(">II", 0x96C14A, 1))
        check(body == b"\x01", "the owned byte was written", body)
        status, body = c.call(OP_MEM_READ, struct.pack(">II", 0x96C0D4, 4))
        check(body == struct.pack(">I", 40), "and its max ammo came with it", body)

        status, _ = c.call(OP_UNLOCK_SET, struct.pack(">BBHI", 0, 3, 0, 12))
        check(status == ST_OK, "UNLOCK_SET ammo=12", status)

        status, body = c.call(OP_UNLOCK_LIST)
        if status == ST_OK:
            _cats, _fields, rows, _n = parse_unlocks(body)
            check(rows[0]["values"][0] == 1 and rows[0]["values"][3] == 12,
                  "UNLOCK_LIST reads the live owned and ammo values",
                  rows[0]["values"])

        status, _ = c.call(OP_UNLOCK_SET, struct.pack(">BBHI", 34, 1, 0, 1))
        check(status == ST_UNSUPPORTED,
              "gold on an entry with no gold byte is UNSUPPORTED", status)

        # ------------------------------------------------------- level flags
        # RaC1's flag region has a format nobody has worked out yet, so the game
        # declares no level-flag hooks and every op comes back UNSUPPORTED. That
        # is the client's cue to hide the panel. RaC2 and RaC3 still answer, and
        # exercise_game checks them.
        status, body = c.call(OP_LEVELFLAGS_GET, bytes([5]))
        check(status == ST_UNSUPPORTED and len(body) == 0,
              "RaC1 LEVELFLAGS_GET is UNSUPPORTED", (status, len(body)))

        status, _ = c.call(OP_LEVELFLAGS_SET, struct.pack(">BBH", 5, 0xCC, 0))
        check(status == ST_UNSUPPORTED, "so is LEVELFLAGS_SET", status)

        status, _ = c.call(OP_LEVELFLAGS_RESET, bytes([5]))
        check(status == ST_UNSUPPORTED, "and so is LEVELFLAGS_RESET", status)

        # -------------------------------------------------- planet load flags
        c.call(OP_MEM_WRITE, struct.pack(">I", RAC1_LEVEL_FLAGS + 3 * 0x10) + b"\x11")
        c.call(OP_MEM_WRITE, struct.pack(">I", RAC1_MISC_FLAGS + 3 * 0x100 + 0x20) + b"\xBB")
        c.call(OP_MEM_WRITE, struct.pack(">I", RAC1_GOLD_BOLTS + 3 * 4)
               + struct.pack(">I", 0xFFFFFFFF))
        c.call(OP_MEM_WRITE, struct.pack(">I", 0x96C142) + b"\x01")   # Heli-Pack

        status, _ = c.call(OP_PLANET_LOAD, bytes([3, 0]))
        check(status == ST_OK, "PLANET_LOAD with no flags", status)
        status, body = c.call(OP_MEM_READ, struct.pack(">II", RAC1_LOAD_PLANET, 8))
        check(body == struct.pack(">II", 1, 3), "the planet request went out", body)
        status, body = c.call(OP_MEM_READ, struct.pack(">II", RAC1_LEVEL_FLAGS + 3 * 0x10, 1))
        check(body == b"\x11", "the level flags were left alone", body)

        status, _ = c.call(OP_PLANET_LOAD,
                           bytes([3, PLANET_FLAG_RESET_LEVELFLAGS
                                  | PLANET_FLAG_RESET_BOLTS]))
        check(status == ST_OK, "PLANET_LOAD with both flags", status)
        status, body = c.call(OP_MEM_READ, struct.pack(">II", RAC1_LEVEL_FLAGS + 3 * 0x10, 1))
        check(body == b"\x00", "bit0 reset the level flags", body)
        status, body = c.call(OP_MEM_READ,
                              struct.pack(">II", RAC1_MISC_FLAGS + 3 * 0x100 + 0x20, 1))
        check(body == b"\x00", "and the misc region with them", body)
        status, body = c.call(OP_MEM_READ, struct.pack(">II", RAC1_GOLD_BOLTS + 3 * 4, 4))
        check(body == struct.pack(">I", 0), "bit1 reset that planet's gold bolts", body)
        status, body = c.call(OP_MEM_READ, struct.pack(">II", 0x96C142, 1))
        check(body == b"\x00", "and Kerwan took the Heli-Pack back", body)

        # --------------------------------------------------------- moby table
        status, body = c.call(OP_MOBY_TABLE)
        if check(status == ST_OK, "MOBY_TABLE answers OK", status):
            tbl, end, stride, _pad = struct.unpack(">IIHH", body)
            check(tbl == 0x0A390A0 and end == 0x0A390A8 and stride == 0x100,
                  "the RaC1 moby pointers and stride come back",
                  (hex(tbl), hex(end), hex(stride)))

        # -------------------------------------------------------- unknown op
        status, body = c.call(OP_UNKNOWN, b"\x01\x02\x03")
        check(status == ST_UNKNOWN_OP and len(body) == 0,
              "an unknown opcode is drained and answered UNKNOWN_OP", (status, len(body)))

        # -------------------------------------------------- memory read/write
        status, _ = c.call(OP_MEM_WRITE, struct.pack(">I", RAC1_BOLTS) + struct.pack(">I", 12345))
        check(status == ST_OK, "MEM_WRITE accepted", status)

        status, body = c.call(OP_MEM_READ, struct.pack(">II", RAC1_BOLTS, 4))
        check(status == ST_OK and body == struct.pack(">I", 12345),
              "MEM_READ returns what MEM_WRITE stored", (status, body))

        # readout[0] should follow the bolt count
        info = fresh_state(c)
        check(info and info["readout"][0] == 12345,
              "readout 0 mirrors the bolt count", info["readout"][0] if info else None)

        # ------------------------------------------------------------ watch
        status, body = c.call(OP_WATCH_ADD, struct.pack(">IB", RAC1_BOLTS, 4))
        watch_id = body[0] if status == ST_OK and body else None
        check(watch_id is not None, "WATCH_ADD returns an id", (status, body))

        status, body = c.call(OP_WATCH_ADD, struct.pack(">IB", RAC1_BOLTS, 4))
        check(status == ST_OK and body and body[0] == watch_id,
              "adding the same watch again returns the same id", body)

        time.sleep(0.2)
        status, body = c.call(OP_GET_STATE)
        _tinfo, watches, _n = parse_telemetry(body)
        found = [w for w in watches if w["id"] == watch_id]
        check(found and found[0]["valid"] == 1 and found[0]["value"] == 12345,
              "the watch reports the live value in telemetry", found)

        # ----------------------------------------------------------- freeze
        status, body = c.call(OP_FREEZE_ADD,
                              struct.pack(">IB3x", RAC1_BOLTS, 4) + struct.pack(">Q", 777))
        freeze_id = body[0] if status == ST_OK and body else None
        check(freeze_id is not None, "FREEZE_ADD returns an id", (status, body))

        time.sleep(0.3)
        status, body = c.call(OP_MEM_READ, struct.pack(">II", RAC1_BOLTS, 4))
        check(body == struct.pack(">I", 777), "the freeze holds the value", body)

        # ------------------------------------------------------ feature set
        # Infinite ammo is feature id 1 and writes one instruction word.
        status, body = c.call(OP_MEM_READ, struct.pack(">II", 0xAA2DC, 4))
        before = body
        status, _ = c.call(OP_FEATURE_SET, struct.pack(">BI", 1, 1))
        check(status == ST_OK, "FEATURE_SET turns infinite ammo on", status)

        status, body = c.call(OP_MEM_READ, struct.pack(">II", 0xAA2DC, 4))
        check(body == struct.pack(">I", 0x60000000),
              "the instruction word was patched", (before, body))

        info = fresh_state(c)
        check(info and (info["toggle_state"] & 2) != 0,
              "telemetry reports the toggle as on",
              hex(info["toggle_state"]) if info else None)

        # The goodies menu is a game-owned byte, so a readout mirrors it too.
        status, _ = c.call(OP_FEATURE_SET, struct.pack(">BI", F_GOODIES, 1))
        check(status == ST_OK, "FEATURE_SET turns the goodies menu on", status)
        info = fresh_state(c)
        check(info and info["readout"][RO_GOODIES] == 1,
              "the goodies readout follows the byte in memory",
              info["readout"][RO_GOODIES] if info else None)
        c.call(OP_FEATURE_SET, struct.pack(">BI", F_GOODIES, 0))

        # ----------------------------------------- protocol 1.3, LIVE toggles
        # A LIVE toggle's state is a byte the game owns, so writing that byte
        # behind qwark's back still moves the bit in toggle_state: the client's
        # checkbox follows the save file rather than what qwark last wrote.
        describe_flags = {}
        status, body = c.call(OP_DESCRIBE)
        if status == ST_OK:
            _g, _gr, _ro, feats, _off = parse_describe(body)
            describe_flags = {f["id"]: f["flags"] for f in feats}

        check(describe_flags.get(F_GOODIES, 0) & FEATURE_FLAG_LIVE,
              "DESCRIBE marks the goodies menu LIVE",
              hex(describe_flags.get(F_GOODIES, 0)))
        for fid in (F_DBG_RATCHET, F_DBG_MOBYS, F_DBG_PARTICLES):
            check(describe_flags.get(fid, 0) & FEATURE_FLAG_LIVE,
                  "DESCRIBE marks debug update id %d LIVE" % fid,
                  hex(describe_flags.get(fid, 0)))
        check(not (describe_flags.get(1, 0) & FEATURE_FLAG_LIVE),
              "and a patch-backed toggle is not LIVE",
              hex(describe_flags.get(1, 0)))

        c.call(OP_MEM_WRITE, struct.pack(">I", RAC1_GOODIES_MENU) + b"\x01")
        info = fresh_state(c, settle=0.4)
        check(info and (info["toggle_state"] & (1 << F_GOODIES)) != 0,
              "a MEM_WRITE of the goodies byte turns the toggle bit on",
              hex(info["toggle_state"]) if info else None)

        # And FEATURE_SET 0 clears both the byte and the bit.
        status, _ = c.call(OP_FEATURE_SET, struct.pack(">BI", F_GOODIES, 0))
        check(status == ST_OK, "FEATURE_SET 0 on a LIVE toggle still writes", status)
        status, body = c.call(OP_MEM_READ, struct.pack(">II", RAC1_GOODIES_MENU, 1))
        check(body == b"\x00", "the byte went to zero", body)
        info = fresh_state(c, settle=0.4)
        check(info and (info["toggle_state"] & (1 << F_GOODIES)) == 0,
              "and the toggle bit followed it back off",
              hex(info["toggle_state"]) if info else None)

        # There is nothing for qwark to re-apply, so there is no auto bit.
        status, _ = c.call(OP_FEATURE_SET_AUTO, bytes([F_GOODIES, 1]))
        check(status == ST_UNSUPPORTED,
              "FEATURE_SET_AUTO on a LIVE toggle is UNSUPPORTED", status)
        info = fresh_state(c)
        check(info and (info["toggle_auto"] & (1 << F_GOODIES)) == 0,
              "and its auto bit stays clear",
              hex(info["toggle_auto"]) if info else None)

        # --------------------------------------------------- position slots
        coords = struct.pack(">fff", 1.5, 2.5, 3.5) + b"\x00" * 18
        c.call(OP_MEM_WRITE, struct.pack(">I", RAC1_COORDS) + coords)

        status, _ = c.call(OP_POS_SAVE, bytes([1]))
        check(status == ST_OK, "POS_SAVE to slot 1", status)

        status, body = c.call(OP_POS_LIST)
        if check(status == ST_OK, "POS_LIST answers OK", status):
            planet, nslots = body[0], body[1]
            slots = []
            for i in range(nslots):
                off = 2 + i * 16
                filled = body[off]
                x, y, z = struct.unpack(">3f", body[off + 4:off + 16])
                slots.append((filled, x, y, z))
            check(nslots == 8, "eight slots per planet", nslots)
            check(slots[1][0] == 1, "slot 1 is filled", slots[1])
            check(abs(slots[1][1] - 1.5) < 0.001 and abs(slots[1][3] - 3.5) < 0.001,
                  "the saved coordinates come back", slots[1])

        # ------------------------------------------- the selected slot persists
        # POS_SELECT writes config.txt on the spot, so a reload with no
        # CONFIG_SAVE in between - the same thing a console reboot does - still
        # comes back with the slot the client picked.
        status, _ = c.call(OP_POS_SELECT, bytes([4]))
        check(status == ST_OK, "POS_SELECT picks slot 4", status)
        sel = fresh_state(c)
        check(sel and sel["slot"] == 4,
              "GET_STATE reports the selected slot",
              sel["slot"] if sel else None)

        status, _ = c.call(OP_CONFIG_RELOAD)
        check(status == ST_OK, "CONFIG_RELOAD re-reads config.txt", status)
        sel = fresh_state(c)
        check(sel and sel["slot"] == 4,
              "the selected slot survived the reload with no CONFIG_SAVE",
              sel["slot"] if sel else None)

        # ------------------------------------------------------------ combo
        status, _ = c.call(OP_COMBO_SET, struct.pack(">B3xI", COMBO_SAVE_POSITION, 0x1005))
        check(status == ST_OK, "COMBO_SET stores a save-position combo", status)

        status, body = c.call(OP_COMBO_LIST)
        combos = {}
        if status == ST_OK:
            for i in range(body[0]):
                off = 1 + i * 8
                combos[body[off]] = struct.unpack(">I", body[off + 4:off + 8])[0]
        check(combos.get(COMBO_SAVE_POSITION) == 0x1005,
              "COMBO_LIST reports it back", combos)

        # Move somewhere recognisable, then press the combo.
        coords2 = struct.pack(">fff", 9.0, 8.0, 7.0) + b"\x00" * 18
        c.call(OP_MEM_WRITE, struct.pack(">I", RAC1_COORDS) + coords2)

        sim.send("pad 0x0")
        time.sleep(0.2)
        sim.send("pad 0x1005")
        time.sleep(0.4)
        sim.send("pad 0x0")
        time.sleep(0.2)

        status, body = c.call(OP_POS_LIST)
        fired = False
        if status == ST_OK:
            off = 2 + 4 * 16
            filled = body[off]
            x, y, z = struct.unpack(">3f", body[off + 4:off + 16])
            fired = filled == 1 and abs(x - 9.0) < 0.001 and abs(z - 7.0) < 0.001
        check(fired, "the controller combo saved a position into slot 4")

        # ------------------------------------------------------ mod listing
        status, body = c.call(OP_MOD_LIST)
        mods = []
        if status == ST_OK:
            n = body[0]
            for i in range(n):
                off = 1 + i * 120
                mods.append({
                    "index": body[off],
                    "flags": body[off + 1],
                    "hash": struct.unpack(">I", body[off + 4:off + 8])[0],
                    "dirname": body[off + 8:off + 40].split(b"\0")[0].decode(),
                    "name": body[off + 40:off + 72].split(b"\0")[0].decode(),
                })
        check(status == ST_OK and len(mods) >= 6,
              "MOD_LIST reports the installed mod fixtures", len(mods))
        check(any(m["name"] == "Hardcore Mode" for m in mods),
              "a mod's #- name came through", [m["name"] for m in mods])

        # ----------------------------------------------- same-title reboot
        sim.send("quit")
        ok, _ = wait_state(c, SESSION_XMB)
        check(ok, "the session returns to XMB on quit")

        sim.send("boot NPEA00385")
        ok, info = wait_state(c, SESSION_INGAME)
        check(ok, "the same title reaches INGAME again")
        check(info and info["generation"] == gen_first + 1,
              "the generation counter advanced",
              (gen_first, info["generation"] if info else None))
        check(info and (info["flags"] & 1) != 0,
              "PREVIOUS_PENDING is set", info["flags"] if info else None)
        check(info and info["toggle_state"] == 0,
              "no toggle is live after the reboot",
              info["toggle_state"] if info else None)

        status, body = c.call(OP_WATCH_LIST)
        ids = []
        if status == ST_OK:
            for i in range(body[0]):
                off = 1 + i * 8
                ids.append(body[off])
        check(watch_id in ids, "the watch survived the reboot with its id", ids)

        status, body = c.call(OP_PREVIOUS_LIST)
        if check(status == ST_OK, "PREVIOUS_LIST answers OK", status):
            toggles = struct.unpack(">Q", body[0:8])[0]
            prev_mods = struct.unpack(">I", body[8:12])[0]
            nfreeze = body[12]
            check((toggles & 2) != 0, "the toggle is in the previous record", hex(toggles))
            check(nfreeze >= 1, "the freeze is in the previous record", nfreeze)

        status, _ = c.call(OP_PREVIOUS_REAPPLY, bytes([0x01 | 0x04]))
        check(status == ST_OK, "PREVIOUS_REAPPLY accepted", status)

        info = fresh_state(c)
        check(info and (info["toggle_state"] & 2) != 0,
              "the toggle came back after REAPPLY",
              hex(info["toggle_state"]) if info else None)
        check(info and (info["flags"] & 1) == 0,
              "PREVIOUS_PENDING cleared", info["flags"] if info else None)

        status, body = c.call(OP_MEM_READ, struct.pack(">II", 0xAA2DC, 4))
        check(body == struct.pack(">I", 0x60000000),
              "and the instruction is patched again in the new process", body)

        # -------------------------------------- RaC2, RaC3 and Deadlocked
        for spec in OTHER_GAMES:
            exercise_game(c, sim, spec)

        # ------------------------------ BCES01503, the disc trilogy
        sim.send("quit")
        ok, _ = wait_state(c, SESSION_XMB)
        check(ok, "quit before the trilogy disc")

        sim.send("boot BCES01503 rac3")
        ok, info = wait_state(c, SESSION_INGAME)
        check(ok, "boot BCES01503 rac3 reaches INGAME", info)
        check(info and info["title"] == "BCES01503",
              "the trilogy title id is reported", info)
        check(info and info["game"] == 3,
              "and the fingerprint says RaC3, not the first candidate",
              info["game"] if info else None)

        status, body = c.call(OP_DESCRIBE)
        if check(status == ST_OK, "DESCRIBE answers under BCES01503", status):
            game, groups, readouts, features, _consumed = parse_describe(body)
            check(game == 3, "and names RaC3", game)
            check(len(features) == 32, "with RaC3's thirty-two features",
                  len(features))

        # -------------------------------------------- unregistered title
        sim.send("quit")
        ok, _ = wait_state(c, SESSION_XMB)
        check(ok, "quit again")

        sim.send("boot NPEA99999")
        # Documented behaviour: an unregistered title keeps the session in XMB.
        never_ingame, info = wait_state(c, SESSION_INGAME, timeout=3.0)
        check(not never_ingame, "an unregistered title never reaches INGAME")
        check(info and info["state"] == SESSION_XMB,
              "it stays in XMB", info["state"] if info else None)
        check(info and info["game"] == 0, "and reports no game",
              info["game"] if info else None)

        status, body = c.call(OP_DESCRIBE)
        check(status == ST_UNSUPPORTED,
              "DESCRIBE is UNSUPPORTED with no game running", status)

        sock.close()

    except Exception as exc:  # noqa: BLE001
        check(False, "the smoke run completed without an exception", repr(exc))
    finally:
        udp.close()
        sim.stop()

    print()
    print("%d passed, %d failed" % (passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
