#!/usr/bin/env python3
"""
End-to-end smoke test for qwark-host.

Starts the simulator, drives its fake console over stdin and talks the real wire
protocol over TCP and UDP telemetry, exactly as docs/PROTOCOL.md describes. Every
port here is picked free at start-up, so a run never lands on the qwark-rpcs3.exe
that is already serving somebody's client on 9673.

Every step prints PASS or FAIL. The exit code is 0 only when everything passed.

Usage:  python test/smoke.py [path-to-qwark-host.exe]
"""

import os
import shutil
import socket
import select
import struct
import subprocess
import sys
import threading
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

sys.path.insert(0, HERE)
from fake_pine import FakePine, STATUS_PAUSED, STATUS_RUNNING  # noqa: E402  the fake RPCS3 IPC server

HOST = "127.0.0.1"

_taken = set()


def free_port():
    """A TCP port nothing is listening on, chosen by the OS.

    Bind port 0 on loopback, read the number back, close. Nothing here binds
    0.0.0.0 - that is what raises the Windows Firewall prompt, and a rule is left
    behind for every Allow - and nothing here uses a fixed port either: 9673 is
    the port a qwark-rpcs3.exe driving somebody's client already holds, and the
    suite used to attach itself to that process instead of the build under test.

    The port is only needed free for the moment the program under test binds it,
    so letting it go again is enough; the set keeps two calls from handing out
    the same number when the OS recycles it.
    """
    for _ in range(64):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            s.bind((HOST, 0))
            port = s.getsockname()[1]
        finally:
            s.close()
        if port not in _taken:
            _taken.add(port)
            return port
    raise RuntimeError("no free port")


PORT = free_port()

# ---------------------------------------------------------------- protocol

# QWARK_BUILD in src/core/proto.h: the module build number, bumped whenever the
# feature tables or any user-visible behaviour change.
QWARK_BUILD = 53

# QWARK_MAX_PAYLOAD, revision 1.11: one frame's payload, request or reply. A
# frame announcing more than this closes the connection.
QWARK_MAX_PAYLOAD = 16384

# What a client actually sends. The protocol says 16000 everywhere, because a
# client that talks to this build and to an older one can use one number for
# both: it is inside 16384 here and inside 65536 there.
CHUNK = 16000

OP_HELLO = 0x0001
OP_HEARTBEAT = 0x0002
OP_PREVIOUS_LIST = 0x0004
OP_PREVIOUS_REAPPLY = 0x0005
OP_PREVIOUS_DISMISS = 0x0006
OP_SUBSCRIBE = 0x0010
OP_UNSUBSCRIBE = 0x0011
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
OP_PATCH_APPLY = 0x0038
OP_POS_SELECT = 0x0040
OP_POS_SAVE = 0x0041
OP_POS_LOAD = 0x0042
OP_POS_LIST = 0x0043
OP_POS_CLEAR = 0x0044
OP_PLANET_LIST = 0x0045
OP_PLANET_LOAD = 0x0047
OP_MOBY_TABLE = 0x0049
# Protocol 1.13: the two ways a client writes a slot it is not standing in.
OP_POS_EDIT = 0x004A
OP_POS_STORE = 0x004B
OP_UNLOCK_LIST = 0x0050
OP_UNLOCK_SET = 0x0051
OP_LEVELFLAGS_GET = 0x0053
OP_LEVELFLAGS_RESET = 0x0054
OP_LEVELFLAGS_SET = 0x0055
OP_MOD_LIST = 0x0060
OP_MOD_LOAD = 0x0061
OP_MOD_UNLOAD = 0x0062
OP_MOD_RESCAN = 0x0064
OP_MOD_INFO = 0x0065
# Revision 1.16: one mod as the words and bytes an RPCS3 patch writes.
OP_MOD_PATCH = 0x0066
# Revision 1.17: the running game's code switches, in SAVEFILE_PATCH's layout.
OP_SWITCH_PATCH = 0x0025

# One Mod row, and the flag that says the console has this one in the game.
MOD_WIRE_SIZE = 120
MOD_FLAG_LOADED = 0x01
MOD_FLAG_PREVIOUS = 0x08
# Revision 1.16: where code cannot be patched, qwark has not looked for it yet.
MOD_FLAG_CHECKING = 0x20
OP_COMBO_SET = 0x0080
OP_COMBO_LIST = 0x0081
OP_COMBO_SUSPEND = 0x0082
OP_COMBO_ENABLE = 0x0083
OP_CONFIG_RELOAD = 0x0090
OP_AUTOSPLIT_EVENTS = 0x00A0
OP_AUTOSPLIT_DESCRIBE = 0x00A1
OP_SAVEFILE_INFO = 0x00B0
OP_SAVEFILE_READ = 0x00B1
OP_SAVEFILE_WRITE = 0x00B2
# Protocol 1.10: the console-side savefile library and the file rename it needs.
OP_FILE_OPEN = 0x0070
OP_FILE_WRITE = 0x0071
OP_FILE_READ = 0x0072
OP_FILE_CLOSE = 0x0073
OP_FILE_DELETE = 0x0074
OP_DIR_CREATE = 0x0076
OP_FILE_RENAME = 0x0079
OP_SAVEFILE_CATEGORIES = 0x00B3
OP_SAVEFILE_LIST = 0x00B4
OP_SAVEFILE_STORE = 0x00B5
OP_SAVEFILE_RESTORE = 0x00B6
OP_SAVEFILE_CATEGORY = 0x00B7
# Revision 1.15: the savefile helper as the words an RPCS3 patch writes.
OP_SAVEFILE_PATCH = 0x00B8
# Revision 1.21: manip categories.
OP_MANIP_LIST = 0x00D0
OP_MANIP_SELECT = 0x00D1
OP_MANIP_OPTIONS = 0x00D2
OP_MANIP_APPLY = 0x00D3
OP_UNKNOWN = 0x7FFF

ST_OK = 0
ST_NOT_INGAME = 1
ST_UNSUPPORTED = 2
ST_BAD_ARG = 3
ST_IO_ERROR = 4
ST_UNKNOWN_OP = 6
ST_NOT_FOUND = 7
ST_BUSY = 8

# SAVEFILE_INFO, revision 1.10: twenty bytes, the last twelve the transfer.
SAVEFILE_INFO_SIZE = 20
SAVEFILE_PENDING_SET_ASIDE = 0x01
SAVEFILE_PENDING_LOAD = 0x02
SAVEFILE_PENDING_TRANSFER = 0x04
SAVEFILE_ERR_NONE = 0
SAVEFILE_ERR_MISSING = 1
SAVEFILE_ERR_IO = 2
SAVEFILE_ERR_SHORT = 3
SAVEFILE_ERR_NO_HELPER = 4
SAVEFILE_ERR_BUSY = 5
# Build 43: the game was not somewhere a save could be taken or put back.
SAVEFILE_ERR_NOT_IDLE = 6
# What the helper leaves in a request byte when it drops one for that reason.
SAVEFILE_HELPER_REFUSED = 0xFE
# Deadlocked's gameMode and pendingGameMode, out of src/games/rac4.h.
R4_GAME_MODE = 0x00B3C5A0
R4_PENDING_GAME_MODE = 0x009C37F4
R4_MODE_IDLE = (struct.pack(">I", 0), struct.pack(">i", -2))
R4_MODE_BUSY = (struct.pack(">I", 6), struct.pack(">i", -2))

# Build 53, revision 1.20: there was no save to act on. RaC1, RaC2 and RaC3 have
# none until the game has saved or loaded once, and their helpers load only a
# save of their own, so a request against a fresh fake process is BUSY with this.
SAVEFILE_ERR_NO_SAVE = 7

# RaC1's savefile helper, from src/games/sfhelper_bins.c: its request bytes and
# its aside buffer, both moved in build 53 (src/games/sfhelper/sf_rac1.h). The
# caves and the hook word are not here, because they come off the wire from
# SAVEFILE_PATCH, which is the point of the op.
RAC1_SF_API_MOD = 0x01080000
RAC1_SF_API_LOAD = 0x01080001
RAC1_SF_API_SETASIDE = 0x01080002
RAC1_SF_ASIDE = 0x00FC0000

# Build 53. What the three games' readiness tests read, out of src/games/rac1.h,
# rac2.h and rac3.h, and a save of each put where the game keeps it: RaC1's
# buffer is the pointer at 0xA10928 plus 0x100000, RaC2's the word at +4 of the
# block at 0x1410E50, RaC3's the word at +4 of the block at 0xCB0A98. A save is
# known by its head, the two block sizes the game's restore insists on, and in
# RaC1 a planet number below 20 at +0x18.
RAC1_SAVE_HEAD = (struct.pack(">II", 0x153C, 0x82A4) + bytes(0x10)
                  + struct.pack(">I", 3))
RAC2_SAVE_HEAD = struct.pack(">II", 0x1D08, 0x84D0)
RAC3_SAVE_HEAD = struct.pack(">II", 0x37B0, 0x84C0)
RAC1_SAVE_WRITES = [
    (0x00A10708, struct.pack(">I", 0)),          # game state: ordinary play
    (0x00A10700, struct.pack(">I", 0)),          # no planet load on its way
    (0x00A10928, struct.pack(">I", 0x00D00000)),
    (0x00E00000, RAC1_SAVE_HEAD),
]
RAC2_SAVE_WRITES = [
    (0x0156B064, struct.pack(">I", 0)),          # gameMode: ordinary play
    (0x01329F90, struct.pack(">i", -2)),         # pendingGameMode: nothing queued
    (0x0156B050, struct.pack(">I", 0)),          # no planet load on its way
    (0x01410E50, struct.pack(">I", 0x01A00000)),
    (0x01A00004, struct.pack(">I", 0x01A10000)),
    (0x01A10000, RAC2_SAVE_HEAD),
]
RAC3_SAVE_WRITES = [
    (0x00EE9334, struct.pack(">I", 0)),          # gameMode: ordinary play
    (0x00EE9310, struct.pack(">I", 0)),          # no planet load on its way
    (0x00CB0A98, struct.pack(">I", 0x01800000)),
    (0x01800004, struct.pack(">I", 0x01810000)),
    (0x01810000, RAC3_SAVE_HEAD),
]
SAVEFILE_NAME_LEN = 32
SAVEFILE_ROW_SIZE = 40
SAVEFILE_CATEGORY_CREATE = 0
SAVEFILE_CATEGORY_DELETE = 1

SESSION_XMB, SESSION_BOOTING, SESSION_INGAME, SESSION_QUITTING = 0, 1, 2, 3

# SessionInfo.flags. Revision 1.6 added the two platform bits: EMULATOR says
# qwark is driving RPCS3 rather than a console, NO_CODE_PATCHES says every
# WRITES_CODE feature is refused here and a client should grey those rows.
# Revision 1.12 added COMBOS_OFF, the state of the COMBO_ENABLE switch.
SESSION_FLAG_PREVIOUS_PENDING = 0x01
SESSION_FLAG_EMULATOR = 0x02
SESSION_FLAG_NO_CODE_PATCHES = 0x04
SESSION_FLAG_COMBOS_OFF = 0x08
# Revision 1.17: the running game's code switches are in it, so the WRITES_CODE
# rows work after all. Only ever beside NO_CODE_PATCHES.
SESSION_FLAG_CODE_SWITCHES = 0x10

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
# Protocol 1.7: the field behind this VALUE is two's complement, `bits` wide.
FEATURE_FLAG_SIGNED = 0x20

# Protocol 1.4: the autosplit event stream. 1.5 added the two load kinds, the
# two timing flags and the param_us that goes with them, and grew EventDesc.
AUTOSPLIT_START, AUTOSPLIT_SPLIT = 1, 2
AUTOSPLIT_RESET, AUTOSPLIT_PAUSE, AUTOSPLIT_RESUME = 3, 4, 5
AUTOSPLIT_LOAD_START, AUTOSPLIT_LOAD_END = 6, 7
AUTOSPLIT_EVENT_SIZE = 16
AUTOSPLIT_DESC_SIZE = 32
AUTOSPLIT_DGRAM_SIZE = 20
AUTOSPLIT_FLAG_DEFAULT = 0x01
AUTOSPLIT_FLAG_ROUTE = 0x02
AUTOSPLIT_FLAG_FLAT = 0x04
AUTOSPLIT_FLAG_NORMALISE = 0x08

DF, RT, FL, NM = (AUTOSPLIT_FLAG_DEFAULT, AUTOSPLIT_FLAG_ROUTE,
                  AUTOSPLIT_FLAG_FLAT, AUTOSPLIT_FLAG_NORMALISE)

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
# Build 53: what a planet load reads to see its end, and the three words fast
# loads patch: (address, the game's word, the patched word). src/games/rac1.c.
RAC1_GAME_STATE = 0xA10708
RAC1_IN_FRONTEND = 0xA15F38
RAC1_FASTLOAD_SITES = [(0x165490, 0x4BFFE519, 0x60000000),
                       (0x160060, 0x546307BE, 0x38600006),
                       (0x1641F8, 0x546307BE, 0x38600006)]
RAC1_DEBUG_MODE = 0x95C5D4
# Build 53: the thirteen words of the routine Deadlocked's fast loads write at
# 0x667DB8 to play a queued movie the way the flight's post-update does
# (src/games/rac4.h), and the dead lobby code the game has there.
RAC4_FL_MOVIE_GAME = list(zip(range(0x667DB8, 0x667DEC, 4), [
    0x7C0803A6, 0xEBC10090, 0xEBE10098, 0x382100A0, 0x4E800020, 0xF821FF61,
    0x7C0802A6, 0xF80100B0, 0xFBE10098, 0x607F0000, 0x38600000, 0xFBC10090,
    0x2C1F0000]))
RAC4_FL_MOVIE_FAST = list(zip(range(0x667DB8, 0x667DEC, 4), [
    0x3CA0010B, 0x80A57FE0, 0x2C050000, 0x4D820020, 0xF821FF91, 0x7C0802A6,
    0xF8010080, 0x4BC3EBC1, 0x4BC4222D, 0xE8010080, 0x7C0803A6, 0x38210070,
    0x4E800020]))
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
    def __init__(self, exe, args=None):
        self.proc = subprocess.Popen(
            [exe] + list(args or []),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=ROOT,
            text=True,
            bufsize=1,
        )
        # Both streams, in one list: the log lines ("pine: ...") are stderr.
        self.lines = []
        self._drain(self.proc.stderr, self.lines)
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


def sim_line(sim, needle, timeout=3.0):
    """The first line the program printed that contains `needle`, or None.

    The banner is printed just after the threads start, so a client can be
    connected before the reader thread has appended it: wait rather than look
    once.
    """
    deadline = time.time() + timeout
    while True:
        for line in list(sim.lines):
            if needle in line:
                return line
        if time.time() >= deadline:
            return None
        time.sleep(0.02)


def sim_arena(sim, timeout=3.0):
    """1 while a connection owns the shared request arena, via the `arena` command.

    Revision 1.11 removed the per-request page allocation entirely: the module
    keeps one 16 KB request buffer and one 16 KB reply buffer and a connection
    holds the pair for the length of one request. What used to be "how many
    pages are live" is "is anybody holding the arena".
    """
    mark = len(sim.lines)
    sim.send("arena")
    deadline = time.time() + timeout
    while time.time() < deadline:
        for line in sim.lines[mark:]:
            if line.startswith("ok arena "):
                return int(line.split()[2])
        time.sleep(0.02)
    return None


def sim_arena_idle(sim, timeout=1.0):
    """0 once the last request has let the arena go, or what it still was when
    `timeout` ran out."""
    deadline = time.time() + timeout
    held = sim_arena(sim)
    while held != 0 and time.time() < deadline:
        time.sleep(0.01)
        held = sim_arena(sim)
    return held


def sim_arena_takes(sim):
    """How many times the arena has ever been taken."""
    mark = len(sim.lines)
    sim.send("arena_takes")
    deadline = time.time() + 3
    while time.time() < deadline:
        for line in sim.lines[mark:]:
            if line.startswith("ok arena_takes "):
                return int(line.split()[2])
        time.sleep(.01)
    raise RuntimeError("no arena count from simulator")


def sim_activity(sim):
    mark = len(sim.lines)
    sim.send("activity")
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        for line in sim.lines[mark:]:
            if line.startswith("ok activity "):
                return tuple(map(int, line.split()[2:]))
        time.sleep(.005)
    raise RuntimeError("no activity snapshot from simulator")


def test_boot_buffers(sim, c, udp_port, udp):
    """Real socket framing, allocation accounting and cancellation at launch."""
    before = sim_arena_takes(sim)
    for _ in range(5):
        c.call(OP_HELLO, b"\x01")
        c.call(OP_HEARTBEAT)
        c.call(OP_GET_STATE)
    check(sim_arena_takes(sim) == before, "control traffic never takes the request arena")

    # Every fixed-buffer opcode, including a SUBSCRIBE whose payload straddles
    # the pause. It is already 1.15 seconds old when the second-long pause starts.
    #
    # Revision 1.11 made QWARK_MAX_CLIENTS 4, and `c` and the stalled bulk
    # connection below take two of the four, so the four whole control frames go
    # down one peer pipelined and the split SUBSCRIBE gets the other. Pipelining
    # is what a client is told it may do, and the ordering guarantee is the same
    # thing being checked: every frame comes back, in order, with its own seq.
    pipelined = [(OP_HELLO, b"\x01"), (OP_HEARTBEAT, b""),
                 (OP_UNSUBSCRIBE, b""), (OP_GET_STATE, b"")]
    subscribe_payload = struct.pack(">H", udp_port)
    peers = [connect() for _ in range(2)]
    for peer in peers:
        Client(peer).call(OP_HEARTBEAT)
    peers[1].sendall(struct.pack(">IHH", 2, 71, OP_SUBSCRIBE) + subscribe_payload[:1])
    time.sleep(1.15)
    partial = connect()
    try:
        # MOD_LOAD, with the entire payload delayed. HELLO now uses fixed buffers.
        partial.sendall(struct.pack(">IHH", 32, 1, 0x61))
        deadline = time.time() + 1
        while sim_arena(sim) == 0 and time.time() < deadline:
            time.sleep(.01)
        check(sim_arena(sim) == 1, "a partial bulk request holds the arena outside boot")
        before = sim_arena_takes(sim)
        sim.send("boot NPEA00385")
        # Hold BOOTING beyond the one-second wait, so the gate test has no timing race.
        with open(os.path.join(ROOT, "src", "games", "rac1.h")) as source:
            fp_header = source.read()
        import re
        fp_addr = re.search(r"#define\s+RAC1_FP_ADDR\s+(0x[0-9A-Fa-f]+)", fp_header).group(1)
        sim.send("poke %s 00000000" % fp_addr)
        deadline = time.monotonic() + 1
        activity = sim_activity(sim)
        while not activity[0] and time.monotonic() < deadline:
            activity = sim_activity(sim)
        check(activity[0] == 1, "IS_INGAME starts the quiet window")
        drain_udp(udp)
        for op, payload in pipelined:
            peers[0].sendall(struct.pack(">IHH", len(payload), 71, op) + payload)
        peers[1].sendall(subscribe_payload[1:])
        ready, _, _ = select.select(peers + [udp], [], [], .65)
        check(not ready, "all control requests and UDP remain silent inside the first second")
        check(sim_activity(sim) == activity, "no PID, title, memory or telemetry activity during the pause")
        check(sim_arena_idle(sim, .1) == 0, "a cancelled bulk request lets the arena go during the quiet window")
        check(sim_arena_takes(sim) == before, "quiet-window traffic never takes the arena")
        for peer, frames in ((peers[0], len(pipelined)), (peers[1], 1)):
            response = Client(peer)
            for _ in range(frames):
                length, seq, status = struct.unpack(">IHH", response.recv_exact(8))
                if length:
                    response.recv_exact(length)
                check(seq == 71 and status == ST_OK,
                      "deferred control frame resumes with its original sequence")
        check(wait_state(c, SESSION_BOOTING)[0], "allocation gate test reaches BOOTING")
        check(sim_arena_idle(sim, 1.0) == 0, "launch cancels an incomplete bulk request and frees the arena")
        try:
            disconnected = partial.recv(1) == b""
        except (ConnectionResetError, ConnectionAbortedError):
            disconnected = True
        check(disconnected, "cancelled bulk connection closes")

        for op, payload in [(OP_DESCRIBE, b""), (OP_FILE_WRITE, bytes(QWARK_MAX_PAYLOAD)),
                            (OP_SAVEFILE_INFO, b"")]:
            status, body = c.call(op, payload)
            check(status == ST_BUSY and body == b"", "boot refuses arena op %d with BUSY" % op, status)
        check(c.call(OP_HELLO, b"\x01")[0] == ST_OK, "HELLO still works in boot")
        check(c.call(OP_HEARTBEAT)[0] == ST_OK, "HEARTBEAT still works after draining a large frame")
        check(c.call(OP_SUBSCRIBE, struct.pack(">H", udp_port))[0] == ST_OK,
              "SUBSCRIBE still works in boot")
        check(c.call(OP_GET_STATE)[0] == ST_OK, "GET_STATE still works in boot")
        check(sim_arena_takes(sim) == before, "boot traffic never took the arena")

        newcomer = connect()
        try:
            try:
                newcomer.sendall(struct.pack(">IHHB", 1, 1, OP_HELLO, 1))
                closed = newcomer.recv(1) == b""
            except (ConnectionResetError, ConnectionAbortedError):
                closed = True
            check(closed, "boot refuses a new connection outright")
        finally:
            newcomer.close()
        sim.send("poke %s 30649ce0" % fp_addr)
        check(wait_state(c, SESSION_INGAME)[0], "normal boot resumes when fingerprint arrives")
        check(c.call(OP_DESCRIBE)[0] == ST_OK, "deferred request succeeds after boot")
        sim.send("quit")
        check(wait_state(c, SESSION_XMB)[0], "allocation probe returns to XMB")
    finally:
        partial.close()
        for peer in peers:
            peer.close()

    # A stalled request also expires without a game transition.
    stalled = connect()
    try:
        stalled.sendall(struct.pack(">IHH", 32, 1, 0x61))
        time.sleep(.1)
        check(sim_arena(sim) == 1, "timeout probe starts holding the arena")
        check(sim_arena_idle(sim, 3.0) == 0, "payload timeout releases the arena without a boot")
        check(c.call(OP_HEARTBEAT)[0] == ST_OK, "other connections survive a stalled request")
    finally:
        stalled.close()


def test_frame_cap(c):
    """Revision 1.11: a frame's payload is at most QWARK_MAX_PAYLOAD.

    The edge on both sides, over a real socket: exactly the cap is a frame like
    any other, and one byte past it closes the connection, which is what 65601
    did before. There is nothing to drain a frame into that will not fit, so the
    connection cannot be kept in step and is dropped rather than desynchronised.
    """
    status, _ = c.call(OP_FILE_WRITE, bytes(QWARK_MAX_PAYLOAD))
    check(status in (ST_NOT_FOUND, ST_BAD_ARG),
          "a payload of exactly 16384 bytes is accepted and answered", status)

    check(c.call(OP_HEARTBEAT)[0] == ST_OK,
          "and the connection is still in step after it")

    over = connect()
    try:
        # The header alone says 16385; the payload never has to be sent.
        over.sendall(struct.pack(">IHH", QWARK_MAX_PAYLOAD + 1, 1, OP_FILE_WRITE))
        over.settimeout(5)
        try:
            closed = over.recv(1) == b""
        except (ConnectionResetError, ConnectionAbortedError):
            closed = True
        except socket.timeout:
            closed = False
        check(closed, "a frame announcing 16385 bytes closes the connection")
    finally:
        over.close()

    check(c.call(OP_HEARTBEAT)[0] == ST_OK,
          "and the other connections are untouched by it")


def test_mem_read_cap(c, addr):
    """MEM_READ at the frame cap and one past it.

    A reply is a frame too, so the largest read is QWARK_MAX_PAYLOAD; one byte
    more is BAD_ARG, the same answer a read past the old 65536 always got.
    """
    status, body = c.call(OP_MEM_READ, struct.pack(">II", addr, QWARK_MAX_PAYLOAD))
    check(status == ST_OK and len(body) == QWARK_MAX_PAYLOAD,
          "MEM_READ of 16384 bytes answers with all of them", (status, len(body)))

    status, body = c.call(OP_MEM_READ, struct.pack(">II", addr, QWARK_MAX_PAYLOAD + 1))
    check(status == ST_BAD_ARG and body == b"",
          "MEM_READ of 16385 is BAD_ARG, as a read past the cap has always been",
          status)

    check(c.call(OP_HEARTBEAT)[0] == ST_OK, "and the connection carries on")


def test_client_limit(c):
    """QWARK_MAX_CLIENTS is 4 since revision 1.11, where it was 8.

    Nothing on the wire carries the number. What a client sees is that a
    connection arriving when every slot is taken is closed as it arrives, which
    is what happened at the ninth before and happens at the fifth now. `c` holds
    one slot, so three more fit and the next is refused.
    """
    extra = []
    try:
        while len(extra) < 8:
            sock = connect()
            try:
                if Client(sock).call(OP_HEARTBEAT)[0] != ST_OK:
                    sock.close()
                    break
            except (OSError, RuntimeError, struct.error):
                sock.close()
                break
            extra.append(sock)
        check(len(extra) == 3,
              "three more clients fit beside this one, and the fifth is closed as it arrives",
              len(extra))
    finally:
        for sock in extra:
            sock.close()

    # Give the connection threads their slots back before anything else knocks.
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            probe = connect()
            probe.close()
            break
        except OSError:
            time.sleep(0.05)
    check(c.call(OP_HEARTBEAT)[0] == ST_OK, "and this one was never disturbed")


def test_arena_sharing(sim, c, addr):
    """Two clients at once, now that they share one request buffer.

    The arena serialises them: the second client's bulk request waits for the
    first rather than being refused, and both get their own correct answer. A
    control op meanwhile never touches the arena and has to be answered while
    the two of them are passing it back and forth.
    """
    peers = [Client(connect()) for _ in range(2)]
    try:
        # Two distinct patterns, so an answer that came from the other client's
        # request would be visible rather than plausible.
        blocks = [bytes([(i * 7 + 1) & 0xFF for i in range(4096)]),
                  bytes([(i * 13 + 200) & 0xFF for i in range(4096)])]
        for n, peer in enumerate(peers):
            status = mem_write(peer, addr + n * 0x8000, blocks[n])
            check(status == ST_OK, "client %d seeds its own block" % n, status)

        results = [None, None]

        def bulk(n):
            try:
                for _ in range(12):
                    status, body = peers[n].call(
                        OP_MEM_READ, struct.pack(">II", addr + n * 0x8000, 4096))
                    if status != ST_OK or body != blocks[n]:
                        results[n] = (status, len(body))
                        return
                results[n] = "ok"
            except Exception as exc:          # noqa: BLE001 - reported as a failure
                results[n] = repr(exc)

        threads = [threading.Thread(target=bulk, args=(n,)) for n in range(2)]
        for t in threads:
            t.start()

        # While those two are hammering the arena, this connection's control ops
        # have to keep coming back: they use their own fixed buffers and never
        # wait for it, which is the same reason they work during a boot.
        control_ok = True
        for _ in range(20):
            if c.call(OP_HEARTBEAT)[0] != ST_OK or c.call(OP_GET_STATE)[0] != ST_OK:
                control_ok = False
                break
        check(control_ok,
              "a control op from another client is answered while two hold the arena")

        for t in threads:
            t.join(30)

        check(results[0] == "ok", "the first client's bulk reads are all its own", results[0])
        check(results[1] == "ok", "and so are the second client's", results[1])
        check(sim_arena_idle(sim, 2.0) == 0, "and the arena is free again afterwards")
    finally:
        for peer in peers:
            peer.sock.close()


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
            # Protocol 1.7: the field width of a VALUE, 0 standing for 32.
            "bits": row[6] or 32,
            "raw_bits": row[6],
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


def parse_autosplit_event(b):
    """The 16-byte Event, the same bytes in the reply and in the datagram."""
    seq, time_ms = struct.unpack(">II", b[0:8])
    return {
        "seq": seq,
        # Revision 1.5: milliseconds since the module started, not a tick count.
        "time_ms": time_ms,
        "kind": b[8],
        "code": b[9],
        "reserved": struct.unpack(">H", b[10:12])[0],
        "arg": struct.unpack(">I", b[12:16])[0],
    }


def parse_autosplit_events(b):
    """AUTOSPLIT_EVENTS: u32 latest_seq, u8 n, Event[n]."""
    latest = struct.unpack(">I", b[0:4])[0]
    n = b[4]
    off = 5
    events = []
    for _ in range(n):
        events.append(parse_autosplit_event(b[off:off + AUTOSPLIT_EVENT_SIZE]))
        off += AUTOSPLIT_EVENT_SIZE
    return latest, events, off


def parse_autosplit_describe(b):
    """AUTOSPLIT_DESCRIBE: u8 n, EventDesc[n]. 32 bytes a row since 1.5."""
    n = b[0]
    off = 1
    rows = []
    for _ in range(n):
        row = b[off:off + AUTOSPLIT_DESC_SIZE]
        rows.append({
            "code": row[0],
            "kind": row[1],
            "flags": row[2],
            "reserved": row[3],
            "param_us": struct.unpack(">I", row[4:8])[0],
            "label": row[8:32].split(b"\0")[0].decode("ascii", "replace"),
        })
        off += AUTOSPLIT_DESC_SIZE
    return rows, off


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


def connect(timeout=10.0, port=PORT):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.create_connection((HOST, port), timeout=5)
            s.settimeout(10)
            return s
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("could not connect to qwark on %d" % port)


# ------------------------------------------- the other three games, end to end

def mem_write(c, addr, data):
    status, _ = c.call(OP_MEM_WRITE, struct.pack(">I", addr) + data)
    return status


def mem_read_u32(c, addr):
    status, body = c.call(OP_MEM_READ, struct.pack(">II", addr, 4))
    if status != ST_OK or len(body) != 4:
        return None
    return struct.unpack(">I", body)[0]


def mem_read_u8(c, addr):
    status, body = c.call(OP_MEM_READ, struct.pack(">II", addr, 1))
    if status != ST_OK or len(body) != 1:
        return None
    return body[0]


def read_u32_until(c, addr, want, timeout=1.0):
    """Reads the word until it is `want` or the time runs out; returns the last read."""
    deadline = time.time() + timeout
    got = mem_read_u32(c, addr)
    while got != want and time.time() < deadline:
        time.sleep(0.02)
        got = mem_read_u32(c, addr)
    return got


# Build 53: Freeze health in every game holds the hit points at the game's own
# maximum, which the health upgrades move. `full`, `more` and `hurt` are the
# words as the game stores them: s32 in RaC1 to RaC3, f32 in Deadlocked.
RAC1_FREEZE_HEALTH = {"id": 2, "health": 0x0096BF88, "max": 0x0071FB28,
                      "full": 5, "more": 8, "hurt": 1}

# Build 53: the four cheats every game has lead its Cheats group, in this order,
# and the client draws them in DESCRIBE's order. The ammo cheat is Infinite
# ammo but in Deadlocked, whose Refill ammo fills the ammo at a mission's start
# on purpose.
FOUR_CHEATS = ["Fast loads", "Ghost Ratchet", "Infinite ammo", "Freeze health"]
FOUR_CHEATS_DL = ["Fast loads", "Ghost Ratchet", "Refill ammo", "Freeze health"]


def cheats_rows(groups, features):
    """The labels of the Cheats group's rows, in DESCRIBE's order."""
    if "Cheats" not in groups:
        return []
    g = groups.index("Cheats")
    return [f["label"] for f in features if f["group"] == g]


def exercise_freeze_health(c, name, fh):
    health, maxw = fh["health"], fh["max"]
    on = struct.pack(">BI", fh["id"], 1)
    off = struct.pack(">BI", fh["id"], 0)

    mem_write(c, maxw, struct.pack(">I", fh["full"]))
    mem_write(c, health, struct.pack(">I", fh["hurt"]))
    status, _ = c.call(OP_FEATURE_SET, on)
    if not check(status == ST_OK, "%s: Freeze health turns on" % name, status):
        return
    got = read_u32_until(c, health, fh["full"])
    check(got == fh["full"], "%s: and holds the hit points at the maximum" % name, got)

    mem_write(c, health, struct.pack(">I", fh["hurt"]))
    got = read_u32_until(c, health, fh["full"])
    check(got == fh["full"], "%s: a hit is put right" % name, got)

    mem_write(c, maxw, struct.pack(">I", fh["more"]))
    got = read_u32_until(c, health, fh["more"])
    check(got == fh["more"], "%s: a health upgrade moves the hold with it" % name, got)

    status, _ = c.call(OP_FEATURE_SET, off)
    check(status == ST_OK, "%s: Freeze health turns off" % name, status)
    mem_write(c, health, struct.pack(">I", fh["hurt"]))
    time.sleep(0.2)
    got = mem_read_u32(c, health)
    check(got == fh["hurt"], "%s: and lets go of the hit points" % name, got)


# ------------------------------------------------------------- autosplitting

def autosplit_events(c, since=0):
    status, body = c.call(OP_AUTOSPLIT_EVENTS, struct.pack(">I", since))
    if status != ST_OK:
        return None, [], 0
    latest, events, consumed = parse_autosplit_events(body)
    return latest, events, consumed if consumed == len(body) else -1


def drain_udp(udp):
    """Throws away whatever telemetry has piled up, so a QE push stands alone."""
    udp.setblocking(False)
    try:
        while True:
            try:
                udp.recvfrom(4096)
            except (BlockingIOError, OSError):
                break
    finally:
        udp.settimeout(2.0)


def telemetry_within(udp, seconds):
    """True when a QWRK telemetry packet arrives within `seconds`."""
    deadline = time.time() + seconds
    try:
        while time.time() < deadline:
            udp.settimeout(max(0.01, deadline - time.time()))
            try:
                data, _addr = udp.recvfrom(4096)
            except socket.timeout:
                return False
            if data[:4] == b"QWRK":
                return True
        return False
    finally:
        udp.settimeout(2.0)


def wait_qe(udp, seq, timeout=2.0):
    """The 20-byte 'QE' datagram carrying this sequence number, or None."""
    deadline = time.time() + timeout
    udp.settimeout(0.25)
    while time.time() < deadline:
        try:
            data, _addr = udp.recvfrom(4096)
        except socket.timeout:
            continue
        except OSError:
            break
        if len(data) != AUTOSPLIT_DGRAM_SIZE or data[:2] != b"QE":
            continue
        if data[2] != 1 or data[3] != 0:
            continue
        ev = parse_autosplit_event(data[4:])
        if ev["seq"] == seq:
            return ev
    return None


def as_wait(c, mark, kind, code, timeout=3.0):
    """The first event past `mark` with this kind and code, or None."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        _latest, events, _n = autosplit_events(c, mark)
        for e in events:
            if e["kind"] == kind and e["code"] == code:
                return e
        time.sleep(0.05)
    return None


def as_absent(c, mark, kind, code, settle=0.5):
    """True when nothing of this kind and code shows up past `mark`."""
    time.sleep(settle)
    _latest, events, _n = autosplit_events(c, mark)
    return not any(e["kind"] == kind and e["code"] == code for e in events)


def exercise_autosplit_steps(c, name, steps):
    """
    Drives a list of watcher steps: each pokes some memory to set the world up,
    pokes some more to make the edge, and says which event must arrive or must
    not. The "timing" rows revision 1.5 added are written this way, and so is
    anything a watcher has to be walked into rather than dropped into.
    """
    for step in steps:
        for addr, value in step.get("poke", []):
            mem_write(c, addr, value)

        # Let the tick thread take the setup in before anything is measured.
        if step.get("poke"):
            time.sleep(0.15)

        mark, _events, _n = autosplit_events(c, 0)

        for addr, value in step.get("then", []):
            mem_write(c, addr, value)

        if step.get("expect") is not None:
            kind, code = step["expect"]
            got = as_wait(c, mark, kind, code)
            if check(got is not None, "%s: %s" % (name, step["what"])):
                if step.get("arg") is not None:
                    check(got["arg"] == step["arg"],
                          "%s: %s carries arg %d" % (name, step["what"], step["arg"]),
                          got["arg"])
        else:
            wanted_gone = step["absent"]
            if isinstance(wanted_gone, tuple):
                wanted_gone = [wanted_gone]
            check(all(as_absent(c, mark, k, code, settle=0.0 if i else 0.5)
                      for i, (k, code) in enumerate(wanted_gone)),
                  "%s: %s" % (name, step["what"]))


def exercise_autosplit(c, udp, name, spec):
    """AUTOSPLIT_DESCRIBE, one real split, and the UDP push that carries it."""
    status, body = c.call(OP_AUTOSPLIT_DESCRIBE)
    if check(status == ST_OK, "%s: AUTOSPLIT_DESCRIBE answers OK" % name, status):
        rows, consumed = parse_autosplit_describe(body)
        want = spec["rows"]
        check(consumed == len(body), "%s: AUTOSPLIT_DESCRIBE parses exactly" % name,
              (consumed, len(body)))
        check(len(rows) == len(want),
              "%s: it lists %d reason codes" % (name, len(want)), len(rows))
        if rows:
            check(rows[0]["code"] == 1 and rows[0]["label"] == "Planet entered",
                  "%s: code 1 is the planet split" % name, rows[0])
            check(rows[0]["flags"] & AUTOSPLIT_FLAG_ROUTE,
                  "%s: and carries the route flag" % name, rows[0]["flags"])
            check(rows[0]["flags"] & AUTOSPLIT_FLAG_DEFAULT,
                  "%s: and is enabled by default" % name, rows[0]["flags"])
            check(all(r["code"] != 0 and r["reserved"] == 0 for r in rows),
                  "%s: every row has a real code and a zero reserved byte" % name)
            check(all((r["flags"] & AUTOSPLIT_FLAG_ROUTE) == 0 for r in rows[1:]),
                  "%s: and only code 1 claims a planet route" % name)

            got = [(r["code"], r["kind"], r["flags"], r["param_us"], r["label"])
                   for r in rows]
            check(got == want,
                  "%s: every row matches code, kind, flags, param and label" % name,
                  [g for g, w in zip(got, want) if g != w])

            # A timing flag always names a parameter, and never both flags.
            bad = [r for r in rows
                   if bool(r["flags"] & (FL | NM)) != bool(r["param_us"]) or
                   (r["flags"] & (FL | NM)) == (FL | NM)]
            check(not bad,
                  "%s: a timing flag comes with exactly one parameter" % name, bad)

    # Some watchers hold state of their own that a poke cannot reach, so a spec
    # may name steps that walk the session into the shape the split needs.
    exercise_autosplit_steps(c, name, spec.get("prime", []))

    # Put the watcher's world where the split condition can be reached from.
    for addr, value in spec["setup"]:
        mem_write(c, addr, value)
    time.sleep(0.1)

    drain_udp(udp)
    mark, _events, _n = autosplit_events(c, 0)
    if mark is None:
        check(False, "%s: AUTOSPLIT_EVENTS answers before the split" % name)
        return

    for addr, value in spec["trigger"]:
        mem_write(c, addr, value)

    got = None
    check_parse = -1
    deadline = time.time() + 3.0
    while time.time() < deadline and got is None:
        _latest, events, consumed = autosplit_events(c, mark)
        check_parse = consumed
        for e in events:
            if e["kind"] == AUTOSPLIT_SPLIT and e["code"] == spec["code"]:
                got = e
                break
        if got is None:
            time.sleep(0.05)

    if not check(got is not None, "%s: the split reaches AUTOSPLIT_EVENTS" % name):
        return

    check(check_parse >= 0, "%s: AUTOSPLIT_EVENTS parses exactly" % name)
    check(got["arg"] == spec["arg"],
          "%s: the split carries planet %d" % (name, spec["arg"]), got["arg"])
    check(got["seq"] > mark, "%s: with a sequence number past the mark" % name,
          (got["seq"], mark))
    check(got["reserved"] == 0, "%s: and a zero reserved halfword" % name)

    ev = wait_qe(udp, got["seq"])
    if check(ev is not None, "%s: the QE datagram arrives over UDP" % name):
        check(ev["kind"] == AUTOSPLIT_SPLIT and ev["code"] == spec["code"] and
              ev["arg"] == spec["arg"] and ev["time_ms"] == got["time_ms"],
              "%s: and carries the same event as the TCP read" % name, ev)

    # since_seq filtering, both sides of the boundary.
    latest, events, _n = autosplit_events(c, got["seq"])
    check(latest == got["seq"] or latest > got["seq"],
          "%s: latest_seq is at least the split" % name, latest)
    check(all(e["seq"] > got["seq"] for e in events),
          "%s: since_seq drops everything up to and including it" % name)

    _latest, events, _n = autosplit_events(c, got["seq"] - 1)
    check(any(e["seq"] == got["seq"] for e in events),
          "%s: and one back still returns it" % name)

    # Revision 1.5: the loads and pauses whose timing the old scripts adjusted.
    exercise_autosplit_steps(c, name, spec.get("timing", []))


# One row per game: what to boot, what DESCRIBE must say, and one of each kind of
# request, so every game gets the same end-to-end pass the RaC1 steps give.
OTHER_GAMES = [
    {
        "title": "NPEA00386",
        "name": "RaC2",
        "game": 2,
        "features": 28,
        "cheats": FOUR_CHEATS + ["Enable debug mode"],
        # The two crash guards' branches (src/games/rac2.c), written as the
        # session enters over the game's own words: (address, game, guard).
        "entry_code": [(0x00086960, 0x7F24C814, 0x490BAAE0),
                       (0x00BDF30C, 0xA07F0034, 0x48562164)],
        # SF_API_SETASIDE from src/games/sfhelper/sf_rac2.h; build 50 moved it
        # out of the executable's code segment.
        "sf_setaside": 0x0131772A,
        # Build 53: a save of the game's own, which a set-aside copies.
        "sf_save": RAC2_SAVE_WRITES,
        # Retired, never renumbered: 32 and 33, the two tempsave manager rows,
        # and (build 53) 15 to 22, the manip buttons and auto-reset toggles,
        # and 4, weapon insta-upgrades.
        "retired": [4, 15, 16, 17, 18, 19, 20, 21, 22, 32, 33],
        # Build 53: the hit points and the game's own maximum, both words.
        "freeze_health": {"id": 2, "health": 0x014816AC, "max": 0x01329A98,
                          "full": 10, "more": 23, "hurt": 3},
        "readouts": 9,
        "planets": 27,
        "planet0": "Aranos",
        "unlocks": 44,
        "categories": 3,
        "unlock0": "Lancer",
        # RaC2 keeps the same item arrays RaC3 does, so slot 1 is the weapon
        # version. Slot 2 was the experience that version earns towards the
        # next one; build 34 stopped offering the column and left the slot
        # unnamed rather than moving Ammo down out of slot 3.
        "fields": [("Owned", UNLOCK_KIND_FLAG, 0),
                   ("Level", UNLOCK_KIND_NUMBER, 4),
                   ("", UNLOCK_KIND_FLAG, 0),
                   ("Ammo", UNLOCK_KIND_NUMBER, 0)],
        "category_fields": {"Weapons": 0xB, "Gadgets": 0x1, "Items": 0x1},
        # The two weapons the game gives no second version. The five RaC1
        # carry-overs have one apiece, bought from Slim Cognito.
        "no_field": [("Zodiac", 1), ("RYNO II", 1)],
        # The Lancer is item id 30 and its V2 is item id 60.
        "unlock_arrays": {"row": "Lancer", "item": 30, "level": 2, "version": 60,
                          "owned": 0x1481A80, "items": 0x1329A40,
                          "ammo": 0x148182C},
        "levelflags": 0x10,
        "coords": 0x147F260,
        # What POS_SAVE produces and what POS_LOAD puts back at "coords".
        "blob_len": 30,
        "blob_main": 30,
        # Bolts, a VALUE with readout 0.
        "value_id": 7,
        "value_addr": 0x1329A90,
        # Protocol 1.7: the VALUEs whose field the game reads as two's complement.
        "signed": [("QE save write-offset", 16), ("Health XP", 32)],
        # The QE feature id, the halfword it writes, and the readout mirroring it.
        "signed_qe": (23, 0x13298CC, 5),
        # "Reset platinum bolts", an ACTION with no helper gate.
        "action_id": 24,
        "action_addr": 0x1562540,
        "action_expect": 0,
        "planet": 5,
        # The classic two-word request: 00000001 000000XX.
        "load_addr": 0x156B050,
        "load_expect": [(0x156B050, 1), (0x156B054, 5)],
        # RaC2 splits on the planet it just arrived at, not on a destination.
        "autosplit": {
            "rows": [
                (1, AUTOSPLIT_SPLIT, DF | RT, 0, "Planet entered"),
                (2, AUTOSPLIT_SPLIT, DF | FL, 116667, "Protopet defeated"),
                (3, AUTOSPLIT_SPLIT, DF, 0, "Aranos 2 Clank swap"),
                (4, AUTOSPLIT_SPLIT, 0, 0, "Maktar arena entry"),
                (5, AUTOSPLIT_SPLIT, 0, 0, "Barlow race entry"),
                (6, AUTOSPLIT_SPLIT, 0, 0, "Endako Clank entry"),
                (7, AUTOSPLIT_SPLIT, 0, 0, "Endako Clank exit"),
                (8, AUTOSPLIT_SPLIT, 0, 0, "Tabora caves"),
                (9, AUTOSPLIT_LOAD_START, DF | FL, 16667, "Load transition: slide"),
                (10, AUTOSPLIT_LOAD_START, DF | FL, 150000, "Load transition: curved"),
                (11, AUTOSPLIT_LOAD_START, DF | FL, 350000, "Load transition: wipe"),
            ],
            "setup": [(0x1329A3C, struct.pack(">I", 0))],
            "trigger": [(0x1329A3C, struct.pack(">I", 5))],
            "code": 1,
            "arg": 5,
            # The load-screen byte, and the three values that cost frames.
            "timing": [
                {"poke": [(0x147A257, b"\x02")],
                 "then": [(0x147A257, b"\x00")],
                 "expect": (AUTOSPLIT_LOAD_START, 9),
                 "what": "load screen 0 emits the slide transition"},
                {"then": [(0x147A257, b"\x01")],
                 "expect": (AUTOSPLIT_LOAD_START, 10),
                 "what": "load screen 1 emits the curved transition"},
                {"then": [(0x147A257, b"\x03")],
                 "expect": (AUTOSPLIT_LOAD_START, 11),
                 "what": "load screen 3 emits the wipe transition"},
                {"then": [(0x147A257, b"\x04")],
                 "absent": [(AUTOSPLIT_LOAD_START, 9),
                            (AUTOSPLIT_LOAD_START, 10),
                            (AUTOSPLIT_LOAD_START, 11)],
                 "what": "an unpriced load screen costs nothing"},
                # A new game loads Aranos before the player state flips to 98,
                # so arriving there is the run starting, never a planet change.
                {"poke": [(0x1329A3C, struct.pack(">I", 7))],
                 "then": [(0x1329A3C, struct.pack(">I", 0))],
                 "absent": (AUTOSPLIT_SPLIT, 1),
                 "what": "the load into Aranos is not a planet split"},
                {"then": [(0x1329A3C, struct.pack(">I", 1))],
                 "expect": (AUTOSPLIT_SPLIT, 1),
                 "arg": 1,
                 "what": "and leaving it for Oozla still splits"},
            ],
        },
    },
    {
        "title": "NPEA00387",
        "name": "RaC3",
        "game": 3,
        "features": 28,
        # Build 53: Infinite ammo was "Freeze ammo".
        "cheats": FOUR_CHEATS + ["Quick-select pause"],
        # SF_API_SETASIDE from src/games/sfhelper/sf_rac3.h.
        "sf_setaside": 0x00D9FF02,
        # Build 53: a save of the game's own, which a set-aside copies.
        "sf_save": RAC3_SAVE_WRITES,
        # Retired, never renumbered: 4, 17, 28, 29, 30 and 33, and (build 53)
        # the manip buttons 16, 18, 19 and 20, and 2, one-hit KO.
        "retired": [2, 4, 16, 17, 18, 19, 20, 28, 29, 30, 33],
        # Build 53: the hit points and the game's own maximum, both words.
        "freeze_health": {"id": 1, "health": 0x00DA5040, "max": 0x00C1E4E4,
                          "full": 30, "more": 200, "hurt": 7},
        "readouts": 12,
        "planets": 37,
        "planet0": "(none)",
        "unlocks": 40,
        "categories": 3,
        "unlock0": "Heli Pack",
        # Slot 1 is UYA's weapon version, not a gold flag, and slot 2 is its XP.
        "fields": [("Owned", UNLOCK_KIND_FLAG, 0),
                   ("Level", UNLOCK_KIND_NUMBER, 8),
                   ("XP", UNLOCK_KIND_NUMBER, 0),
                   ("Ammo", UNLOCK_KIND_NUMBER, 0)],
        # Build 13: the level, XP and ammo slots belong to the weapons. A gadget
        # and a vid comic are owned or not owned and carry nothing else.
        "category_fields": {"Weapons": 0xF, "Gadgets and items": 0x1,
                            "Vid comics": 0x1},
        # The Suck Cannon carries no ammo the game counts.
        "no_field": [("Suck Cannon", 3)],
        # Build 13: id 0, the Bomb Glove, retired because UYA cannot reach the
        # item in game. Ids are never renumbered, so the rest kept theirs.
        "unlock_retired": [(0, "Bomb Glove")],
        "levelflags": 0x10,
        "coords": 0xDA2870,
        "blob_len": 30,
        "blob_main": 30,
        "value_id": 7,
        "value_addr": 0xC1E4DC,
        "signed": [("QE offset", 16), ("Health XP", 32)],
        "signed_qe": (14, 0xC1E2C0, 6),
        # "Reset all titanium bolts".
        "action_id": 25,
        "action_addr": 0xECE53D,
        "action_expect": 0,
        "planet": 5,
        "load_addr": 0xEE9310,
        "load_expect": [(0xEE9310, 1), (0xEE9314, 5)],
        # The Fast loads toggle, a code patch since it stopped being two data
        # words (src/games/rac3.h, RAC3_FASTLOAD_SITE): two sites in a pass of
        # the transition loop, the update's argument at 0x27B49C and the frame's
        # head at 0x27B550, become branches into the cave at 0x9807F0, and the
        # load request word clearing with the mode out of 6, once the mode has
        # been 6, is the end of a load.
        "fastload": {
            "toggle": 38,
            "site": 0x27B49C,
            "site_word": 0x63430000,
            "branch": 0x48705354,
            "draw": 0x27B550,
            "draw_word": 0x4BE9F7F5,
            "draw_branch": 0x4870533C,
            "cave": 0x9807F0,
            "cave_first": 0x3CA000DA,
            "request": 0xEE9310,
            "mode": 0xEE9334,
            "planet": 9,
        },
        # RaC3 splits when the destination planet changes to another real planet.
        "autosplit": {
            "rows": [
                (1, AUTOSPLIT_SPLIT, DF | RT, 0, "Planet entered"),
                (5, AUTOSPLIT_SPLIT, DF, 0, "Biobliterator defeated"),
                (2, AUTOSPLIT_SPLIT, 0, 0, "LDF entered"),
                (4, AUTOSPLIT_SPLIT, 0, 0, "Koros bolt 2"),
                (3, AUTOSPLIT_SPLIT, 0, 0, "Tyhrraguise obtained"),
                (6, AUTOSPLIT_LOAD_START, DF | FL, 1000000, "Long load"),
            ],
            "setup": [(0x00C1E438, struct.pack(">I", 4)),
                      (0x00EE9314, struct.pack(">I", 0))],
            "trigger": [(0x00EE9314, struct.pack(">I", 7))],
            "code": 1,
            "arg": 7,
            # The long load, and the ignore list on both ends of the trip.
            "timing": [
                {"poke": [(0x00C1E438, struct.pack(">I", 4)),
                          (0x00EE9314, struct.pack(">I", 0)),
                          (0x00D99117, b"\x00")],
                 "then": [(0x00EE9314, struct.pack(">I", 7)),
                          (0x00D99117, b"\x01")],
                 "expect": (AUTOSPLIT_LOAD_START, 6),
                 "arg": 7,
                 "what": "a long load between ordinary planets counts"},
                {"then": [(0x00D99117, b"\x00")],
                 "expect": (AUTOSPLIT_LOAD_END, 6),
                 "arg": 7,
                 "what": "and leaving the loading screen closes it"},
                {"poke": [(0x00EE9314, struct.pack(">I", 20))],
                 "then": [(0x00D99117, b"\x01")],
                 "absent": (AUTOSPLIT_LOAD_START, 6),
                 "what": "a long load to an ignored planet does not"},
                {"then": [(0x00D99117, b"\x00")],
                 "absent": (AUTOSPLIT_LOAD_END, 6),
                 "what": "and nothing closes a load that never opened"},
                # The script's comment says it does not split going to Veldin;
                # the test under it is its RaC1 ancestor's, where Veldin is 0.
                {"poke": [(0x00EE9314, struct.pack(">I", 0))],
                 "then": [(0x00EE9314, struct.pack(">I", 1))],
                 "absent": (AUTOSPLIT_SPLIT, 1),
                 "what": "the load to Veldin is not a planet split"},
                {"poke": [(0x00EE9314, struct.pack(">I", 0))],
                 "then": [(0x00EE9314, struct.pack(">I", 2))],
                 "expect": (AUTOSPLIT_SPLIT, 1),
                 "arg": 2,
                 "what": "and the load to Florana still splits"},
            ],
        },
    },
    {
        "title": "NPEA00423",
        "name": "Deadlocked",
        "game": 4,
        "features": 16,
        "cheats": FOUR_CHEATS_DL,
        # SF_API_SETASIDE and SF_API_LOAD from src/games/sfhelper/sf_rac4.h.
        "sf_setaside": 0x015CD71F,
        "sf_load": 0x015CD71E,
        # Build 43: Deadlocked raises nothing at a game that is not idle, and a
        # fresh fake process reads as a mode change queued, so it is told so.
        "sf_idle": [(R4_GAME_MODE, R4_MODE_IDLE[0]),
                    (R4_PENDING_GAME_MODE, R4_MODE_IDLE[1])],
        # Retired, never renumbered: 16, "Reset all weapon levels", and (build
        # 53) 12, "Act tune bosses", which is the NG+ manip group now.
        "retired": [12, 16],
        # Build 53: the hero's hit points and maximum, both floats: 15.0, 200.0
        # and 5.0. The old form froze the integer 200 into the float.
        "freeze_health": {"id": 4, "health": 0x010D7250, "max": 0x010D7354,
                          "full": 0x41700000, "more": 0x43480000,
                          "hurt": 0x40A00000},
        "readouts": 8,
        "planets": 16,
        "planet0": "(unused)",
        "unlocks": 28,
        "categories": 3,
        "unlock0": "Dual Vipers",
        # A bot upgrade is one owned byte. A weapon is an entry in g_GadgetData:
        # a level halfword and an ammo halfword. The level on the wire is the one
        # the game shows, V1..V99, and the halfword behind it is one lower; a
        # locked weapon holds -1 there and reads out as level 0. The two pairs of
        # boots are entries 17 and 18 of that same table with the same encoding,
        # but no version and no magazine, so they are owned and nothing else.
        "fields": [("Owned", UNLOCK_KIND_FLAG, 0),
                   ("Level", UNLOCK_KIND_NUMBER, 99),
                   ("Ammo", UNLOCK_KIND_NUMBER, 0),
                   ("", UNLOCK_KIND_FLAG, 0)],
        "category_fields": {"Weapons": 0x7, "Gadgets": 0x1, "Bot upgrades": 0x1},
        # The rows that live in g_GadgetData have ids 32 plus the gadget index,
        # so the address follows from the id and the indices are stated only in
        # the SPRX. An entry is 68 bytes and the level and ammo halfwords are
        # its first four.
        "unlock_entry": {"row": "Dual Vipers", "base": 0x00B2B760,
                         "id_base": 32, "stride": 68, "level_bias": -1},
        # Deadlocked has never had a level-flag region.
        "levelflags": None,
        "coords": 0x10D44D0,
        # Deadlocked adds the two camera floats, and puts 0x20 back at "coords".
        "blob_len": 40,
        "blob_main": 32,
        "value_id": 7,
        "value_addr": 0x9C32E8,
        # "Unlock all planets" writes the rank, Liberator, as one byte.
        "action_id": 11,
        "action_addr": 0x9C331F,
        "action_expect": 4,
        "planet": 4,
        # Deadlocked writes the planet then a 1 into a second word.
        "load_addr": 0xB36DD0,
        "load_expect": [(0xB36DD0, 4), (0xB36DCC, 1)],
        # Build 53: Fast loads, which a planet load forces, are six instruction
        # words on RaC2's model (src/games/rac4.h) and a routine in a cave: the
        # autosave's call and its two arguments become FadeToBlack(2, 0), the
        # transition update's call becomes the routine, which plays a queued
        # movie after the segment's actors are deleted, as the flight's
        # post-update does, the copy of its answer sends the loop round once,
        # and that one time round draws no frame. The game's mode leaving the
        # space transition (6) puts a forced set back.
        "code_fastload": {
            "toggle": 2,
            "mode": 0xB3C5A0,
            "game": [(0x2A8AF4, 0x48120261), (0x2A8AE8, 0x38600000),
                     (0x2A8AEC, 0x80846DD0)] + RAC4_FL_MOVIE_GAME +
                    [(0x2A8C50, 0x4E9E0421), (0x2A8C5C, 0x607B0000),
                     (0x2A8CC8, 0x38600001)],
            "fast": [(0x2A8AF4, 0x4BEAC045), (0x2A8AE8, 0x38600002),
                     (0x2A8AEC, 0x38800000)] + RAC4_FL_MOVIE_FAST +
                    [(0x2A8C50, 0x483BF169), (0x2A8C5C, 0x3B600001),
                     (0x2A8CC8, 0x48000020)],
        },
        # Deadlocked splits when a load starts for a real planet while in game,
        # and only once the session has left the main menu: the watcher keeps its
        # own origin planet because the game's word still holds the save's one
        # after a boot. The prime step is that first load, which must not split.
        "autosplit": {
            "rows": [
                (1, AUTOSPLIT_SPLIT, DF | RT, 0, "Planet entered"),
                (2, AUTOSPLIT_SPLIT, DF, 0, "Vox defeated"),
                (3, AUTOSPLIT_PAUSE, DF | NM, 14800000, "Quit to XMB"),
            ],
            # The game's own planet word stays 4 throughout, which is the save
            # leftover that used to make the load out of the menu a split.
            "prime": [
                {"poke": [(0x00B36DCC, struct.pack(">I", 0)),
                          (0x00B1F460, struct.pack(">I", 1)),
                          (0x00B1F46C, struct.pack(">I", 1)),
                          (0x009C3240, struct.pack(">I", 4)),
                          (0x00B36DD0, struct.pack(">I", 0))],
                 "then": [(0x00B36DCC, struct.pack(">I", 1))],
                 "absent": (AUTOSPLIT_SPLIT, 1),
                 "what": "a load back to the main menu does not split"},
                {"poke": [(0x00B36DCC, struct.pack(">I", 0)),
                          (0x00B36DD0, struct.pack(">I", 4))],
                 "then": [(0x00B36DCC, struct.pack(">I", 1))],
                 "absent": (AUTOSPLIT_SPLIT, 1),
                 "what": "and the first load out of the menu does not either"},
            ],
            "setup": [(0x00B36DCC, struct.pack(">I", 0)),
                      (0x00B1F460, struct.pack(">I", 1)),
                      (0x00B1F46C, struct.pack(">I", 1)),
                      (0x009C3240, struct.pack(">I", 4)),
                      (0x00B36DD0, struct.pack(">I", 6))],
            "trigger": [(0x00B36DCC, struct.pack(">I", 1))],
            "code": 1,
            "arg": 6,
            # One flight whose load request word rises twice. The second edge
            # names the planet the watcher already has as the origin, so it is
            # the same load coming round again rather than a second change.
            "timing": [
                {"poke": [(0x00B36DCC, struct.pack(">I", 0))],
                 "then": [(0x00B36DCC, struct.pack(">I", 1))],
                 "absent": (AUTOSPLIT_SPLIT, 1),
                 "what": "a second load request for the same planet does not split again"},
                {"poke": [(0x00B36DCC, struct.pack(">I", 0)),
                          (0x00B36DD0, struct.pack(">I", 7))],
                 "then": [(0x00B36DCC, struct.pack(">I", 1))],
                 "expect": (AUTOSPLIT_SPLIT, 1),
                 "arg": 7,
                 "what": "and the flight after it still splits"},
            ],
        },
    },
]


def exercise_savefile(c, name, save_aside_id, setaside_addr=None, idle=None, save=None):
    """
    Protocol 1.9: the savefile block, against whichever game is up.

    The helper's own code never runs here - the simulator is a fake console with
    no PowerPC in it - so what this checks is the wire: that INFO reports the
    game as supported and the helper as installed, that the buffer round-trips
    through WRITE and READ in chunks, that the SAVE_ASIDE action raises the
    pending bit, and that the bounds are the documented ones.

    `setaside_addr` is the game's own request byte. Given one, this also plays
    the helper's part in clearing it, which is the only way to see the settle
    window from the wire.

    `idle` is the state a game that refuses a request while it is busy has to
    be put in first (build 43). A fresh fake process is all zeroes, which for
    Deadlocked reads as a queued mode change, so nothing would be raised at it
    at all without this.

    `save` is a save of the game's own put where the game keeps it (build 53):
    RaC1, RaC2 and RaC3 set aside and load nothing else, and a fresh fake process,
    like a game that has not saved yet, has none.
    """
    if idle:
        for addr, data in idle:
            mem_write(c, addr, data)
    if save:
        # Build 53, revision 1.20: before the game has a save of its own a
        # set-aside is refused at once, and INFO's error byte says why.
        status, _ = c.call(OP_FEATURE_TRIGGER, bytes([save_aside_id]))
        check(status == ST_BUSY,
              "%s: a set-aside before the game has a save is BUSY" % name, status)
        info = savefile_info(c)
        check(info is not None and info["error"] == SAVEFILE_ERR_NO_SAVE
              and info["pending"] == 0,
              "%s: with error 7, no save, and nothing outstanding" % name, info)
        for addr, data in save:
            mem_write(c, addr, data)
    status, body = c.call(OP_SAVEFILE_INFO)
    if not check(status == ST_OK and len(body) == SAVEFILE_INFO_SIZE,
                 "%s: SAVEFILE_INFO answers with twenty bytes" % name,
                 (status, len(body))):
        return

    supported, installed, running, pending = body[0], body[1], body[2], body[3]
    size = struct.unpack(">I", body[4:8])[0]

    check(supported == 1, "%s: the game is supported" % name, supported)
    if name == "RaC1":
        check(installed == 0, "RaC1: initial status query leaves the helper uninstalled", installed)
    initial_installed = installed
    check(running == 0, "%s: the fake console runs no PowerPC, so it is idle" % name,
          running)
    check(pending == 0, "%s: and no request is outstanding" % name, pending)
    check(size > 0, "%s: the aside buffer has a size" % name, size)

    # Revision 1.15: on a console the op is informational, and it asks nothing
    # of the game, so INFO says what it said before.
    check_savefile_patch(c, name)
    status, body = c.call(OP_SAVEFILE_INFO)
    check(status == ST_OK and body[1] == initial_installed,
          "%s: SAVEFILE_PATCH installs nothing" % name)

    # A chunk at the front and a chunk that runs off the end.
    pattern = bytes((i * 7 + 3) & 0xFF for i in range(4096))
    status, _ = c.call(OP_SAVEFILE_WRITE, struct.pack(">I", 0) + pattern)
    check(status == ST_OK, "%s: SAVEFILE_WRITE takes a chunk" % name, status)

    status, body = c.call(OP_SAVEFILE_READ, struct.pack(">II", 0, len(pattern)))
    check(status == ST_OK and body == pattern,
          "%s: SAVEFILE_READ hands the same bytes back" % name,
          (status, len(body)))
    status, body = c.call(OP_SAVEFILE_INFO)
    check(status == ST_OK and body[1] == initial_installed,
          "%s: buffer I/O does not install helper hooks" % name)

    status, body = c.call(OP_SAVEFILE_READ, struct.pack(">II", size - 16, 4096))
    check(status == ST_OK and len(body) == 16,
          "%s: a read past the end is trimmed to what is left" % name,
          (status, len(body)))

    status, _ = c.call(OP_SAVEFILE_READ, struct.pack(">II", size, 4))
    check(status == ST_BAD_ARG,
          "%s: an offset at the end of the buffer is BAD_ARG" % name, status)

    status, _ = c.call(OP_SAVEFILE_WRITE, struct.pack(">I", size - 2) + b"\x01\x02\x03")
    check(status == ST_BAD_ARG,
          "%s: a write that runs past the end is BAD_ARG" % name, status)

    status, _ = c.call(OP_SAVEFILE_READ, struct.pack(">II", 0, QWARK_MAX_PAYLOAD + 1))
    check(status == ST_BAD_ARG,
          "%s: a read longer than one frame is BAD_ARG" % name, status)

    # The SAVE_ASIDE action, and the bit a client polls until the helper clears.
    status, _ = c.call(OP_FEATURE_TRIGGER, bytes([save_aside_id]))
    if check(status == ST_OK, "%s: the SAVE_ASIDE action fires" % name, status):
        status, body = c.call(OP_SAVEFILE_INFO)
        check(status == ST_OK and body[1] == 1 and body[3] == 0x01,
              "%s: and INFO's pending bit0 says the set-aside is outstanding" % name,
              body[3] if body else None)

    if setaside_addr is None:
        return

    # Build 11: the helper clears its request byte only after the copy is over,
    # and qwark then holds the bit for a settle window of ticks, so a client that
    # sees the bit clear knows the whole buffer is there to read.
    mem_write(c, setaside_addr, b"\x00")
    status, body = c.call(OP_SAVEFILE_INFO)
    check(status == ST_OK and body[3] == 0x01,
          "%s: a request byte that has just read zero is still outstanding" % name,
          body[3] if body else None)

    time.sleep(0.5)
    status, body = c.call(OP_SAVEFILE_INFO)
    check(status == ST_OK and body[3] == 0,
          "%s: and a whole settle window of zeroes is what clears it" % name,
          body[3] if body else None)


def exercise_savefile_not_idle(c, name, setaside_addr, load_addr,
                               save_aside_id, load_aside_id):
    """
    Build 43, over the wire: nothing is raised at a game that is not idle.

    Deadlocked works on its own save data from inside the game, so a request
    raised in a cutscene, a planet load or a menu either copies a save the game
    is still writing or hands its loader a buffer something else is about to
    walk over. qwark reads the game's two mode words before it writes a request
    byte and answers BUSY with SAVEFILE_INFO error 6 instead, and the helper
    makes the same test on the frame it would act and drops the request there.

    The helper's own frame is not something a PC can run, so the simulator
    stands in for it here exactly as it stands in for the cleared request byte
    everywhere else in this file: the refusal code goes into the request byte by
    hand, and what is checked is what qwark does with it.
    """
    busy = [(R4_GAME_MODE, R4_MODE_BUSY[0]), (R4_PENDING_GAME_MODE, R4_MODE_BUSY[1])]
    idle = [(R4_GAME_MODE, R4_MODE_IDLE[0]), (R4_PENDING_GAME_MODE, R4_MODE_IDLE[1])]

    mem_write(c, setaside_addr, b"\x00")
    mem_write(c, load_addr, b"\x00")
    for addr, data in busy:
        mem_write(c, addr, data)

    status, _ = c.call(OP_FEATURE_TRIGGER, bytes([save_aside_id]))
    check(status == ST_BUSY,
          "%s: the SAVE_ASIDE action is BUSY while the game is not idle" % name, status)
    status, _ = c.call(OP_FEATURE_TRIGGER, bytes([load_aside_id]))
    check(status == ST_BUSY, "%s: and so is LOAD_ASIDE" % name, status)

    info = savefile_info(c)
    check(info is not None and info["error"] == SAVEFILE_ERR_NOT_IDLE,
          "%s: SAVEFILE_INFO says the game was not idle" % name,
          info["error"] if info else None)
    check(info is not None and info["pending"] == 0,
          "%s: with nothing left outstanding" % name,
          info["pending"] if info else None)
    check(mem_read_u8(c, setaside_addr) == 0 and mem_read_u8(c, load_addr) == 0,
          "%s: and neither request byte was written" % name)

    status, _ = c.call(OP_SAVEFILE_STORE,
                       fixed32("busy") + fixed32("busy.sav"))
    check(status == ST_BUSY, "%s: SAVEFILE_STORE is BUSY too" % name, status)
    status, _ = c.call(OP_SAVEFILE_RESTORE,
                       fixed32("busy") + fixed32("busy.sav"))
    check(status == ST_BUSY,
          "%s: and SAVEFILE_RESTORE, ahead of even looking for the file" % name, status)
    info = savefile_info(c)
    check(info is not None and info["error"] == SAVEFILE_ERR_NOT_IDLE,
          "%s: both for the same documented reason" % name,
          info["error"] if info else None)

    # The same ops, from a game that is sitting still.
    for addr, data in idle:
        mem_write(c, addr, data)

    status, _ = c.call(OP_FEATURE_TRIGGER, bytes([save_aside_id]))
    check(status == ST_OK, "%s: from an idle game the action fires" % name, status)
    check(mem_read_u8(c, setaside_addr) == 1,
          "%s: and raises the request byte as it always did" % name)

    # And the other half: the helper found the game busy on its own frame.
    mem_write(c, setaside_addr, bytes([SAVEFILE_HELPER_REFUSED]))
    time.sleep(0.2)
    check(mem_read_u8(c, setaside_addr) == 0,
          "%s: qwark clears the helper's refusal code out of the byte" % name)
    info = savefile_info(c)
    check(info is not None and info["pending"] == 0,
          "%s: the request is dropped at once, not held for a later frame" % name,
          info["pending"] if info else None)
    check(info is not None and info["error"] == SAVEFILE_ERR_NOT_IDLE,
          "%s: and the client is told why" % name,
          info["error"] if info else None)


def savefile_patch(c, op=OP_SAVEFILE_PATCH):
    """
    SAVEFILE_PATCH, revision 1.15:
    (status, stamp, [(addr, 4 value bytes), ...], [(addr, value), ...], body).

    The reply is u16 n, u16 pad, u32 stamp, n (u32 addr, u32 value) pairs, then
    u16 nb, u16 pad and nb (u32 addr, u8 value, u8 pad[3]) single bytes. The
    stamp is the CRC-32 of every byte after it exactly as it came in, so a
    client can check it with zlib.crc32 and nothing else. SWITCH_PATCH
    (revision 1.17) is laid out the same way, and `op` asks for it instead.
    """
    status, body = c.call(op)
    if status != ST_OK or len(body) < 8:
        return status, None, None, None, body
    n, _, stamp = struct.unpack(">HHI", body[:8])
    off = 8
    words = []
    for _ in range(n):
        words.append((struct.unpack(">I", body[off:off + 4])[0], body[off + 4:off + 8]))
        off += 8
    single = []
    if off + 4 <= len(body):
        nb = struct.unpack(">H", body[off:off + 2])[0]
        off += 4
        for _ in range(nb):
            single.append((struct.unpack(">I", body[off:off + 4])[0], body[off + 4]))
            off += 8
    return status, stamp, words, single, body


def check_savefile_patch(c, name):
    """That SAVEFILE_PATCH answers and describes itself, whatever the platform."""
    status, stamp, words, single, body = savefile_patch(c)
    if not check(status == ST_OK, "%s: SAVEFILE_PATCH answers" % name, status):
        return None
    n = struct.unpack(">H", body[:2])[0]
    check(body[2:4] == b"\0\0", "%s: SAVEFILE_PATCH's pad is 0" % name)
    check(len(single) == 3 and len(body) == 8 + n * 8 + 4 + 3 * 8,
          "%s: it is %d word pairs, then three single bytes behind their own head"
          % (name, n), (len(body), len(single)))
    tail = body[8 + n * 8:]
    check(tail[2:4] == b"\0\0" and all(tail[4 + i * 8 + 4:4 + i * 8 + 8] == b"\0\0\0\0"
                                       for i in range(len(single))),
          "%s: the byte entries are 0 with their pads 0" % name)
    check(stamp == (zlib.crc32(body[8:]) & 0xFFFFFFFF),
          "%s: and its stamp is the CRC-32 of everything after it" % name, hex(stamp))
    check(len(body) <= QWARK_MAX_PAYLOAD, "%s: and it fits one frame" % name, len(body))
    check(all(addr % 4 == 0 for addr, _ in words),
          "%s: every word is at an aligned address" % name)
    return stamp, words, single


def savefile_info(c):
    """SAVEFILE_INFO as a dict, revision 1.10."""
    status, body = c.call(OP_SAVEFILE_INFO)
    if status != ST_OK or len(body) != SAVEFILE_INFO_SIZE:
        return None
    size, done, total = struct.unpack(">III", body[4:16])
    return {
        "supported": body[0], "installed": body[1], "running": body[2],
        "pending": body[3], "size": size, "done": done, "total": total,
        "error": body[16],
    }


def savefile_names(payload, row_size):
    """The `u8 n` and the fixed 32-byte names of a CATEGORIES or LIST reply."""
    n = payload[0]
    rows = []
    for i in range(n):
        off = 1 + i * row_size
        rows.append(payload[off:off + row_size])
    return [r[:SAVEFILE_NAME_LEN].split(b"\0")[0].decode() for r in rows], rows


def fixed32(text):
    return text.encode().ljust(SAVEFILE_NAME_LEN, b"\0")


def wait_transfer(c, timeout=10.0):
    """Polls INFO until the transfer bit clears, as a client's save loop does."""
    deadline = time.time() + timeout
    info = savefile_info(c)
    while info is not None and (info["pending"] & SAVEFILE_PENDING_TRANSFER):
        if time.time() > deadline:
            return info
        time.sleep(0.02)
        info = savefile_info(c)
    return info


def read_console_file(c, path):
    """FILE_OPEN read, FILE_READ to the end, FILE_CLOSE. None when missing."""
    status, body = c.call(OP_FILE_OPEN, b"\x00" + path.encode())
    if status != ST_OK:
        return None

    handle = body[:4]
    data = b""
    while True:
        status, chunk = c.call(OP_FILE_READ, handle + struct.pack(">I", CHUNK))
        if status != ST_OK:
            break
        data += chunk
        if len(chunk) < CHUNK:
            break

    c.call(OP_FILE_CLOSE, handle)
    return data


def mod_list(c):
    """MOD_LIST, as the rows a client reads out of it."""
    status, body = c.call(OP_MOD_LIST)
    mods = []
    if status == ST_OK and body:
        for i in range(body[0]):
            off = 1 + i * MOD_WIRE_SIZE
            mods.append({
                "index": body[off],
                "flags": body[off + 1],
                "hash": struct.unpack(">I", body[off + 4:off + 8])[0],
                "dirname": body[off + 8:off + 40].split(b"\0")[0].decode(),
                "name": body[off + 40:off + 72].split(b"\0")[0].decode(),
                "version": body[off + 72:off + 88].split(b"\0")[0].decode(),
                "author": body[off + 88:off + 120].split(b"\0")[0].decode(),
            })
    return status, mods


def mod_row(c, dirname):
    """One mod's MOD_LIST row, or None."""
    _, mods = mod_list(c)
    return next((m for m in mods if m["dirname"] == dirname), None)


def mod_patch_page(c, index, page, request=None):
    """
    MOD_PATCH, revision 1.19: one page, (status, pages, stamp, [(addr, 4 value
    bytes)], [(addr, value)], body). The request is u8 index, u8 pad, u16 page
    unless `request` gives the payload itself. SAVEFILE_PATCH's layout with the
    pad after n being pages: u16 n, u16 pages, u32 stamp, n (u32 addr,
    u32 value) rows, u16 nb, u16 pad, nb (u32 addr, u8 value, u8 pad[3]) bytes.
    The stamp is the same on every page: the CRC-32 of one unbounded reply after
    its stamp, which for a one-page mod is everything after it.
    """
    payload = request if request is not None else struct.pack(">BBH", index, 0, page)
    status, body = c.call(OP_MOD_PATCH, payload)
    if status != ST_OK or len(body) < 8:
        return status, None, None, None, None, body
    n, pages, stamp = struct.unpack(">HHI", body[:8])
    off = 8
    rows = []
    for _ in range(n):
        rows.append((struct.unpack(">I", body[off:off + 4])[0], body[off + 4:off + 8]))
        off += 8
    single = []
    if off + 4 <= len(body):
        nb = struct.unpack(">H", body[off:off + 2])[0]
        off += 4
        for _ in range(nb):
            single.append((struct.unpack(">I", body[off:off + 4])[0], body[off + 4]))
            off += 8
    return status, pages, stamp, rows, single, body


def mod_patch(c, index):
    """
    MOD_PATCH as a client before revision 1.19 sends it, u8 index and three zero
    pads, which is page 0: (status, stamp, rows, single, body), as above.
    """
    status, _, stamp, rows, single, body = mod_patch_page(
        c, index, 0, request=bytes([index, 0, 0, 0]))
    return status, stamp, rows, single, body


def write_console_file(c, path, data):
    status, body = c.call(OP_FILE_OPEN, b"\x01" + path.encode())
    if status != ST_OK:
        return False

    handle = body[:4]
    for off in range(0, len(data), CHUNK):
        status, _ = c.call(OP_FILE_WRITE, handle + data[off:off + CHUNK])
        if status != ST_OK:
            c.call(OP_FILE_CLOSE, handle)
            return False

    c.call(OP_FILE_CLOSE, handle)
    return True


def exercise_savefile_library(c, name, setaside_addr, load_addr, save=None, head=b""):
    """
    Protocol 1.10: the savefile library on the console, over the real wire.

    The console keeps /dev_hdd0/qwark/savefiles/<TITLEID>/<category>/<name>.sav
    and copies between one of those and the helper's aside buffer on its own
    tick thread, so a save is never streamed from the PC again. Nothing here
    runs a line of the helper - the simulator has no PowerPC in it - so this
    plays the helper's part by clearing its request bytes, which is exactly what
    exercise_savefile does for the plain requests.

    The CRC the console reports is checked against Python's zlib.crc32, because
    the whole point of it is that the two sides agree about a file byte for byte.

    Build 53: `save` puts a save of the game's own where the game keeps it, for
    the STORE's set-aside, and `head` goes in front of the pattern the buffer is
    filled with, so the file the RESTORE copies back is a save the game takes.
    """
    if save:
        for addr, data in save:
            mem_write(c, addr, data)
    root = "/dev_hdd0/qwark/savefiles/NPEA00385"
    category = "smoke"
    folder = "%s/%s" % (root, category)

    info = savefile_info(c)
    if info is None or not info["supported"]:
        check(False, "%s: SAVEFILE_INFO answers before the library is used" % name, info)
        return

    size = info["size"]

    # ------------------------------------------------------------ categories
    status, body = c.call(OP_SAVEFILE_CATEGORIES)
    check(status == ST_OK, "%s: SAVEFILE_CATEGORIES answers" % name, status)

    status, _ = c.call(OP_SAVEFILE_CATEGORY,
                       bytes([SAVEFILE_CATEGORY_CREATE]) + fixed32(category))
    check(status == ST_OK, "%s: a category is created" % name, status)

    status, body = c.call(OP_SAVEFILE_CATEGORIES)
    names, _ = savefile_names(body, SAVEFILE_NAME_LEN)
    check(status == ST_OK and category in names,
          "%s: and comes back in the categories" % name, names)

    status, _ = c.call(OP_SAVEFILE_CATEGORY,
                       bytes([SAVEFILE_CATEGORY_CREATE]) + fixed32("../escape"))
    check(status == ST_BAD_ARG,
          "%s: a category name with a separator is BAD_ARG" % name, status)

    # ----------------------------------------------------------------- STORE
    # A whole buffer of a known pattern, so the CRC the console computes has
    # something to be right about.
    payload = bytes(((i * 31 + 7) & 0xFF) for i in range(CHUNK))
    payload = head + payload[len(head):]
    written = b""
    for off in range(0, size, CHUNK):
        chunk = payload[:min(CHUNK, size - off)]
        status, _ = c.call(OP_SAVEFILE_WRITE, struct.pack(">I", off) + chunk)
        if status != ST_OK:
            break
        written += chunk
    check(status == ST_OK and len(written) == size,
          "%s: the aside buffer is filled with a known pattern" % name,
          (status, len(written)))
    want_crc = zlib.crc32(written) & 0xFFFFFFFF

    status, _ = c.call(OP_SAVEFILE_STORE, fixed32(category) + fixed32("run1.txt"))
    check(status == ST_BAD_ARG,
          "%s: STORE of a name that is not a .sav is BAD_ARG" % name, status)

    status, _ = c.call(OP_SAVEFILE_STORE, fixed32(category) + fixed32("run1.sav"))
    check(status == ST_OK, "%s: SAVEFILE_STORE is accepted" % name, status)

    info = savefile_info(c)
    check(info and (info["pending"] & SAVEFILE_PENDING_TRANSFER),
          "%s: INFO says a transfer is in flight" % name, info)
    check(info and info["total"] == size and info["done"] == 0,
          "%s: with the whole buffer to go" % name, info)

    # One transfer at a time, and the listing steps aside for it.
    status, _ = c.call(OP_SAVEFILE_STORE, fixed32(category) + fixed32("run2.sav"))
    check(status == ST_BUSY, "%s: a second STORE during one is BUSY" % name, status)
    status, _ = c.call(OP_SAVEFILE_LIST, fixed32(category))
    check(status == ST_BUSY, "%s: and so is a listing" % name, status)

    # The helper's part: the request byte goes back to zero and the settle
    # window runs out, and only then does the copy start.
    mem_write(c, setaside_addr, b"\x00")
    info = wait_transfer(c)
    check(info and (info["pending"] & SAVEFILE_PENDING_TRANSFER) == 0,
          "%s: the transfer finishes" % name, info)
    check(info and info["done"] == size and info["error"] == SAVEFILE_ERR_NONE,
          "%s: with every byte copied and no error" % name, info)

    # ------------------------------------------------------------------ LIST
    status, body = c.call(OP_SAVEFILE_LIST, fixed32(category))
    names, rows = savefile_names(body, SAVEFILE_ROW_SIZE)
    check(status == ST_OK and names == ["run1.sav"],
          "%s: SAVEFILE_LIST reports the stored save" % name, (status, names))
    if rows:
        row_size, row_crc = struct.unpack(">II", rows[0][SAVEFILE_NAME_LEN:])
        check(row_size == size, "%s: with the size of the buffer" % name, row_size)
        check(row_crc == want_crc,
              "%s: and a CRC that agrees with Python's zlib.crc32" % name,
              (hex(row_crc), hex(want_crc)))

    data = read_console_file(c, "%s/run1.sav" % folder)
    check(data is not None and len(data) == size,
          "%s: the file itself is the size of the buffer" % name,
          None if data is None else len(data))
    check(data is not None and (zlib.crc32(data) & 0xFFFFFFFF) == want_crc,
          "%s: and holds the bytes the buffer held" % name)

    sidecar = read_console_file(c, "%s/run1.sav.sum" % folder)
    check(sidecar == ("%08x" % want_crc).encode(),
          "%s: the .sum sidecar holds the same eight hex digits" % name, sidecar)

    # --------------------------------------------------------------- RESTORE
    # Something else in the buffer first, so a restore that did nothing shows.
    c.call(OP_SAVEFILE_WRITE, struct.pack(">I", 0) + bytes(4096))
    mem_write(c, load_addr, b"\x00")

    status, _ = c.call(OP_SAVEFILE_RESTORE, fixed32(category) + fixed32("run1.sav"))
    check(status == ST_OK, "%s: SAVEFILE_RESTORE is accepted" % name, status)

    # It copies the file in and only then raises the load at the game.
    deadline = time.time() + 10.0
    raised = b"\x00"
    while time.time() < deadline:
        status, raised = c.call(OP_MEM_READ, struct.pack(">II", load_addr, 1))
        if status == ST_OK and raised == b"\x01":
            break
        time.sleep(0.02)
    check(raised == b"\x01",
          "%s: the load request goes out once the file is in the buffer" % name, raised)

    status, body = c.call(OP_SAVEFILE_READ, struct.pack(">II", 0, 4096))
    check(status == ST_OK and body == payload[:4096],
          "%s: and the buffer holds the file again" % name, status)

    mem_write(c, load_addr, b"\x00")
    info = wait_transfer(c)
    check(info and info["error"] == SAVEFILE_ERR_NONE,
          "%s: the restore ends when the load has settled" % name, info)

    # ------------------------------------------------------- the two refusals
    status, _ = c.call(OP_SAVEFILE_RESTORE, fixed32(category) + fixed32("gone.sav"))
    check(status == ST_NOT_FOUND, "%s: restoring a missing file is NOT_FOUND" % name, status)
    info = savefile_info(c)
    check(info and info["error"] == SAVEFILE_ERR_MISSING,
          "%s: and the error byte says which" % name, info)
    check(info and (info["pending"] & SAVEFILE_PENDING_TRANSFER) == 0,
          "%s: with nothing raised at the game" % name, info)

    check(write_console_file(c, "%s/short.sav" % folder, b"not a save"),
          "%s: a short file is uploaded with the file ops" % name)
    status, _ = c.call(OP_SAVEFILE_RESTORE, fixed32(category) + fixed32("short.sav"))
    check(status == ST_BAD_ARG, "%s: restoring it is BAD_ARG" % name, status)
    info = savefile_info(c)
    check(info and info["error"] == SAVEFILE_ERR_SHORT,
          "%s: with the short-file error" % name, info)

    # A file the client uploaded has no sidecar, so the listing sums it once.
    status, body = c.call(OP_SAVEFILE_LIST, fixed32(category))
    names, rows = savefile_names(body, SAVEFILE_ROW_SIZE)
    check(status == ST_OK and sorted(names) == ["run1.sav", "short.sav"],
          "%s: the listing reports the uploaded file too" % name, names)
    for row in rows:
        if row[:SAVEFILE_NAME_LEN].split(b"\0")[0] != b"short.sav":
            continue
        row_size, row_crc = struct.unpack(">II", row[SAVEFILE_NAME_LEN:])
        check(row_crc == (zlib.crc32(b"not a save") & 0xFFFFFFFF),
              "%s: with a CRC computed from the file itself" % name, hex(row_crc))

    # ----------------------------------------------------------- FILE_RENAME
    def rename(src, dst):
        return c.call(OP_FILE_RENAME,
                      struct.pack(">H", len(src)) + src.encode() + dst.encode())[0]

    check(rename("%s/short.sav" % folder, "%s/renamed.sav" % folder) == ST_OK,
          "%s: FILE_RENAME moves a file" % name)
    check(read_console_file(c, "%s/renamed.sav" % folder) == b"not a save",
          "%s: under its new name" % name)
    check(rename("%s/short.sav" % folder, "%s/other.sav" % folder) == ST_NOT_FOUND,
          "%s: renaming a file that is not there is NOT_FOUND" % name)
    check(rename("%s/renamed.sav" % folder, "%s/run1.sav" % folder) == ST_BAD_ARG,
          "%s: and renaming onto a name that exists is refused" % name)
    check(rename("/dev_hdd0/qwark/x", "/etc/passwd") == ST_BAD_ARG,
          "%s: a path outside /dev_hdd0 is BAD_ARG" % name)

    # ------------------------------------------------------ delete the lot
    status, _ = c.call(OP_SAVEFILE_CATEGORY,
                       bytes([SAVEFILE_CATEGORY_DELETE]) + fixed32(category))
    check(status != ST_OK,
          "%s: a category with saves in it is not deleted" % name, status)

    for leaf in ("run1.sav", "run1.sav.sum", "renamed.sav"):
        c.call(OP_FILE_DELETE, ("%s/%s" % (folder, leaf)).encode())

    status, _ = c.call(OP_SAVEFILE_CATEGORY,
                       bytes([SAVEFILE_CATEGORY_DELETE]) + fixed32(category))
    check(status == ST_OK,
          "%s: with the saves gone the category goes, sidecars and all" % name, status)

    status, body = c.call(OP_SAVEFILE_CATEGORIES)
    names, _ = savefile_names(body, SAVEFILE_NAME_LEN)
    check(status == ST_OK and category not in names,
          "%s: and it is out of the categories" % name, names)


def exercise_game(c, sim, spec, udp=None):
    name = spec["name"]

    sim.send("quit")
    ok, _ = wait_state(c, SESSION_XMB)
    check(ok, "%s: the previous game quit" % name)

    sim.send("boot %s" % spec["title"])
    # The game's own words wherever qwark writes code of its own as it enters.
    for addr, word, _want in spec.get("entry_code", []):
        sim.send("poke 0x%08X %08x" % (addr, word))
    ok, info = wait_state(c, SESSION_INGAME)
    check(ok, "%s: %s reaches INGAME" % (name, spec["title"]), info)
    if not ok:
        return
    check(info["game"] == spec["game"],
          "%s: the game byte is right" % name, info["game"])
    if spec.get("entry_code"):
        got = [mem_read_u32(c, a) for a, _w, _want in spec["entry_code"]]
        check(got == [want for _a, _w, want in spec["entry_code"]],
              "%s: the session put its own code in as it entered" % name,
              [hex(v) for v in got])

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
        if spec.get("cheats"):
            rows = cheats_rows(groups, features)
            check(rows == spec["cheats"],
                  "%s: Cheats leads with the four cheats every game has, in order" % name,
                  rows)
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

        # Protocol 1.9: that pair, and the block that moves the bytes.
        if save_aside:
            exercise_savefile(c, name, save_aside[0]["id"], spec.get("sf_setaside"),
                              spec.get("sf_idle"), spec.get("sf_save"))
            if spec.get("sf_idle"):
                exercise_savefile_not_idle(c, name, spec["sf_setaside"],
                                           spec["sf_load"], save_aside[0]["id"],
                                           load_aside[0]["id"])

        # Protocol 1.7: only a VALUE carries a field width, and a SIGNED row
        # leaves min and max at 0 because the width is already the range.
        check(all(f["raw_bits"] == 0 for f in features if f["kind"] != FEATURE_VALUE),
              "%s: only a VALUE names a field width" % name)
        for f in features:
            if not f["flags"] & FEATURE_FLAG_SIGNED:
                continue
            check(f["kind"] == FEATURE_VALUE and f["raw_bits"] in (8, 16, 32)
                  and f["min"] == 0 and f["max"] == 0,
                  "%s: the signed '%s' is a VALUE, %d bits, unbounded"
                  % (name, f["label"], f["bits"]), f)
        for label, bits in spec.get("signed", []):
            row = next((f for f in features if f["label"] == label), None)
            check(row is not None and row["flags"] & FEATURE_FLAG_SIGNED
                  and row["raw_bits"] == bits,
                  "%s: '%s' is SIGNED and %d bits wide" % (name, label, bits), row)

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

        # What a row declares is decided by its category: a client draws the
        # column from the descriptors and the cell from the row's own bits, so
        # a category with no level, XP or ammo leaves those cells empty. The
        # no_field rows below are the per-row exceptions to this.
        exceptions = {}
        for row_name, slot in spec.get("no_field", []):
            exceptions.setdefault(row_name, 0)
            exceptions[row_name] |= 1 << slot
        wrong = []
        for r in rows:
            cat = cats[r["category"]] if r["category"] < len(cats) else None
            want = spec.get("category_fields", {}).get(cat)
            if want is None:
                wrong.append((r["name"], cat))
                continue
            want &= ~exceptions.get(r["name"], 0)
            if r["fields"] != want:
                wrong.append((r["name"], hex(r["fields"]), hex(want)))
        check(not wrong,
              "%s: every row declares the slots its category has" % name, wrong)

        # A retired unlock id keeps its number out of use: no row carries it,
        # nothing is renumbered into it, and UNLOCK_SET on it is BAD_ARG.
        for gone_id, gone_name in spec.get("unlock_retired", []):
            check(all(r["id"] != gone_id for r in rows),
                  "%s: the retired unlock id %d is absent" % (name, gone_id))
            check(all(r["name"] != gone_name for r in rows),
                  "%s: and so is the %s" % (name, gone_name))
            status, _ = c.call(OP_UNLOCK_SET,
                               struct.pack(">BBHI", gone_id, 0, 0, 1))
            check(status == ST_BAD_ARG,
                  "%s: UNLOCK_SET on it is BAD_ARG" % name, status)

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

        # And one round trip through UNLOCK_SET, on whatever id row 0 carries:
        # a game with a retired id no longer has one at 0.
        row0 = rows[0]["id"] if rows else 0
        status, _ = c.call(OP_UNLOCK_SET, struct.pack(">BBHI", row0, 0, 0, 1))
        check(status == ST_OK, "%s: UNLOCK_SET owned=1" % name, status)
        status, body = c.call(OP_UNLOCK_LIST)
        if status == ST_OK:
            _cats, _fields, rows, _n = parse_unlocks(body)
            check(rows and rows[0]["values"][0] == 1,
                  "%s: and it reads back live" % name,
                  rows[0]["values"] if rows else None)

        # A row whose number slots are a struct in game memory: the level and
        # the ammo go out as halfwords in the entry the row's id points at, and
        # come back through UNLOCK_LIST in the slots that named them. A game
        # whose halfword is not the level it shows says so with level_bias, and
        # the round trip still has to end on the number that was sent.
        spec_entry = spec.get("unlock_entry")
        if spec_entry:
            row = next((r for r in rows if r["name"] == spec_entry["row"]), None)
            if check(row is not None,
                     "%s: %s is in the table" % (name, spec_entry["row"])):
                addr = (spec_entry["base"] +
                        (row["id"] - spec_entry["id_base"]) * spec_entry["stride"])
                stored = 7 + spec_entry.get("level_bias", 0)
                status, _ = c.call(OP_UNLOCK_SET,
                                   struct.pack(">BBHI", row["id"], 1, 0, 7))
                check(status == ST_OK, "%s: UNLOCK_SET level=7" % name, status)
                status, _ = c.call(OP_UNLOCK_SET,
                                   struct.pack(">BBHI", row["id"], 2, 0, 250))
                check(status == ST_OK, "%s: UNLOCK_SET ammo=250" % name, status)

                status, body = c.call(OP_MEM_READ, struct.pack(">II", addr, 4))
                check(body == struct.pack(">HH", stored, 250),
                      "%s: both halfwords are at the head of the entry" % name,
                      body)

                status, body = c.call(OP_UNLOCK_LIST)
                if status == ST_OK:
                    _cats, _fields, rows, _n = parse_unlocks(body)
                    row = next((r for r in rows if r["name"] == spec_entry["row"]),
                               None)
                    check(row and row["values"][:3] == (1, 7, 250),
                          "%s: and UNLOCK_LIST reads owned, level and ammo back"
                          % name, row["values"] if row else None)

        # The other shape a game's number slots take: parallel arrays indexed by
        # an item id, one cell per slot, and a level that is the id of the
        # version in use rather than a count. Each write has to land in the cell
        # its own array names and come back through UNLOCK_LIST as the number
        # that was sent.
        spec_arrays = spec.get("unlock_arrays")
        if spec_arrays:
            row = next((r for r in rows if r["name"] == spec_arrays["row"]), None)
            if check(row is not None,
                     "%s: %s is in the table" % (name, spec_arrays["row"])):
                item = spec_arrays["item"]
                # Slot 2 is the experience word, which a game offers only where
                # the spec names an exp array: RaC2 dropped the column in build
                # 34, and the slot then reads back 0 and refuses a write.
                exp = spec_arrays.get("exp")
                sent = (1, spec_arrays["level"], 4242 if exp else 0, 250)
                ok = True
                for slot, value in enumerate(sent):
                    want = ST_OK if slot != 2 or exp else ST_UNSUPPORTED
                    status, _ = c.call(OP_UNLOCK_SET,
                                       struct.pack(">BBHI", row["id"], slot, 0,
                                                   value))
                    ok = check(status == want,
                               "%s: UNLOCK_SET slot %d = %d" % (name, slot, value),
                               status) and ok

                if ok:
                    check(mem_read_u8(c, spec_arrays["owned"] + item) == 1,
                          "%s: the owned byte is at its item id" % name)
                    check(mem_read_u8(c, spec_arrays["items"] + item) ==
                          spec_arrays["version"],
                          "%s: and the item array holds the version's own id" % name)
                    if exp:
                        check(mem_read_u32(c, exp + item * 4) == 4242,
                              "%s: the XP landed in the exp array" % name)
                    check(mem_read_u32(c, spec_arrays["ammo"] + item * 4) == 250,
                          "%s: and the ammo in the ammo array" % name)

                    status, body = c.call(OP_UNLOCK_LIST)
                    if status == ST_OK:
                        _cats, _fields, rows, _n = parse_unlocks(body)
                        row = next((r for r in rows
                                    if r["name"] == spec_arrays["row"]), None)
                        check(row and row["values"] == sent,
                              "%s: and UNLOCK_LIST reads all four back" % name,
                              row["values"] if row else None)

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

    # ------------------------------------ FEATURE_SET on a signed VALUE, 1.7
    if spec.get("signed_qe"):
        qe_id, qe_addr, qe_readout = spec["signed_qe"]

        # -1 travels as the low sixteen bits of the u32, and has to reach the
        # game as a halfword or a negative offset never lands.
        status, _ = c.call(OP_FEATURE_SET, struct.pack(">BI", qe_id, 0xFFFF))
        if check(status == ST_OK, "%s: FEATURE_SET sends the QE offset -1" % name, status):
            status, body = c.call(OP_MEM_READ, struct.pack(">II", qe_addr, 2))
            check(status == ST_OK and body == b"\xFF\xFF",
                  "%s: and both bytes of the halfword are 0xFF" % name, body)
            info = fresh_state(c)
            check(info and info["readout"][qe_readout] == 0xFFFF,
                  "%s: the readout carries the raw halfword, not a sign-extended word" % name,
                  info["readout"][qe_readout] if info else None)

        status, _ = c.call(OP_FEATURE_SET, struct.pack(">BI", qe_id, 0x8000))
        if check(status == ST_OK, "%s: and the most negative offset too" % name, status):
            status, body = c.call(OP_MEM_READ, struct.pack(">II", qe_addr, 2))
            check(status == ST_OK and body == b"\x80\x00",
                  "%s: which lands as 0x8000" % name, body)

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

    # --------------------------------- POS_EDIT and POS_STORE, revision 1.13
    # A slot with something recognisable behind the coordinates, so an edit
    # that trampled the rotation would show.
    blob_len, blob_main = spec["blob_len"], spec["blob_main"]
    tail = bytes((0x40 + i) & 0xFF for i in range(blob_main - 12))
    mem_write(c, spec["coords"], struct.pack(">3f", 10.0, 50.0, -10.0) + tail)
    status, _ = c.call(OP_POS_SAVE, bytes([2]))
    check(status == ST_OK, "%s: POS_SAVE fills slot 2" % name, status)

    status, _ = c.call(OP_POS_EDIT, struct.pack(">B3x3f", 2, 11.0, 22.0, 33.0))
    if check(status == ST_OK, "%s: POS_EDIT rewrites slot 2" % name, status):
        status, body = c.call(OP_POS_LIST)
        off = 2 + 2 * 16
        x, y, z = struct.unpack(">3f", body[off + 4:off + 16])
        check(abs(x - 11.0) < 0.01 and abs(y - 22.0) < 0.01 and abs(z - 33.0) < 0.01,
              "%s: and POS_LIST reports the three new floats" % name, (x, y, z))

    mem_write(c, spec["coords"], b"\x00" * blob_main)
    status, _ = c.call(OP_POS_LOAD, bytes([2]))
    if check(status == ST_OK, "%s: POS_LOAD after the edit runs" % name, status):
        status, got = c.call(OP_MEM_READ, struct.pack(">II", spec["coords"], blob_main))
        x, y, z = struct.unpack(">3f", got[0:12])
        check(abs(x - 11.0) < 0.01 and abs(y - 22.0) < 0.01 and abs(z - 33.0) < 0.01,
              "%s: putting the edited coordinates in the game" % name, (x, y, z))
        check(got[12:blob_main] == tail,
              "%s: while the rotation words are the ones that were captured" % name,
              got[12:blob_main].hex())

    c.call(OP_POS_CLEAR, bytes([3]))
    status, _ = c.call(OP_POS_EDIT, struct.pack(">B3x3f", 3, 1.0, 2.0, 3.0))
    check(status == ST_NOT_FOUND,
          "%s: POS_EDIT on an empty slot is NOT_FOUND" % name, status)
    status, _ = c.call(OP_POS_EDIT, struct.pack(">B3x3f", 8, 1.0, 2.0, 3.0))
    check(status == ST_BAD_ARG, "%s: POS_EDIT on slot 8 is BAD_ARG" % name, status)

    # POS_STORE: a whole blob, of exactly this game's length.
    imported = (struct.pack(">3f", 70.0, 80.0, 90.0) +
                bytes((0x90 + i) & 0xFF for i in range(blob_len - 12)))
    status, _ = c.call(OP_POS_STORE, bytes([5, blob_len, 0, 0]) + imported)
    if check(status == ST_OK, "%s: POS_STORE fills slot 5" % name, status):
        status, body = c.call(OP_POS_LIST)
        off = 2 + 5 * 16
        x, y, z = struct.unpack(">3f", body[off + 4:off + 16])
        check(body[off] == 1 and abs(x - 70.0) < 0.01 and abs(y - 80.0) < 0.01 and
              abs(z - 90.0) < 0.01,
              "%s: and POS_LIST shows the imported slot" % name, (body[off], x, y, z))
        mem_write(c, spec["coords"], b"\x00" * blob_main)
        status, _ = c.call(OP_POS_LOAD, bytes([5]))
        if check(status == ST_OK, "%s: POS_LOAD on the imported slot runs" % name, status):
            status, got = c.call(OP_MEM_READ, struct.pack(">II", spec["coords"], blob_main))
            check(got == imported[:blob_main],
                  "%s: writing the stored blob back into the game" % name, got.hex())

    status, _ = c.call(OP_POS_STORE,
                       bytes([5, blob_len - 1, 0, 0]) + imported[:blob_len - 1])
    check(status == ST_BAD_ARG,
          "%s: a blob one byte short is BAD_ARG" % name, status)
    status, _ = c.call(OP_POS_STORE, bytes([8, blob_len, 0, 0]) + imported)
    check(status == ST_BAD_ARG, "%s: POS_STORE on slot 8 is BAD_ARG" % name, status)

    c.call(OP_POS_CLEAR, bytes([2]))
    c.call(OP_POS_CLEAR, bytes([5]))

    # -------------------------------------------------------- planet load
    for addr, _want in spec["load_expect"]:
        mem_write(c, addr, struct.pack(">I", 0))
    status, _ = c.call(OP_PLANET_LOAD, bytes([spec["planet"], 0]))
    if check(status == ST_OK, "%s: PLANET_LOAD accepted" % name, status):
        for addr, want in spec["load_expect"]:
            check(mem_read_u32(c, addr) == want,
                  "%s: the load request word at 0x%X is %d" % (name, addr, want),
                  mem_read_u32(c, addr))

    # ------------------------------- Deadlocked's code fast loads (build 53)
    #
    # The planet load above forced them on, over whatever the fake process held
    # at those words; leaving the space transition puts that back. Then the
    # game's own words go in, and the checkbox and a second forced load
    # are watched against them.
    if spec.get("code_fastload"):
        cf = spec["code_fastload"]

        def fl_words():
            return [mem_read_u32(c, a) for a, _w in cf["game"]]

        game = [w for _a, w in cf["game"]]
        fast = [w for _a, w in cf["fast"]]
        mode_was = mem_read_u32(c, cf["mode"])

        mem_write(c, cf["mode"], struct.pack(">I", 6))
        time.sleep(0.15)
        mem_write(c, cf["mode"], struct.pack(">I", 0))
        time.sleep(0.15)
        for addr, word in cf["game"]:
            mem_write(c, addr, struct.pack(">I", word))

        status, _ = c.call(OP_FEATURE_SET, struct.pack(">BI", cf["toggle"], 1))
        if check(status == ST_OK, "%s: the Fast loads checkbox turns on" % name, status):
            check(fl_words() == fast,
                  "%s: the autosave, the update, the loop's exit and its frame "
                  "are the patch's words" % name,
                  [hex(w) for w in fl_words()])
        status, _ = c.call(OP_FEATURE_SET, struct.pack(">BI", cf["toggle"], 0))
        check(status == ST_OK and fl_words() == game,
              "%s: and off, the game's own words are back" % name,
              [hex(w) for w in fl_words()])

        status, _ = c.call(OP_PLANET_LOAD, bytes([spec["planet"], 0]))
        if check(status == ST_OK, "%s: a second PLANET_LOAD" % name, status):
            check(fl_words() == fast, "%s: forces the same words on" % name,
                  [hex(w) for w in fl_words()])
            mem_write(c, cf["mode"], struct.pack(">I", 6))
            time.sleep(0.15)
            check(fl_words() == fast,
                  "%s: they stay through the space transition" % name,
                  [hex(w) for w in fl_words()])
            mem_write(c, cf["mode"], struct.pack(">I", 0))
            time.sleep(0.15)
            check(fl_words() == game,
                  "%s: and go when the game leaves it" % name,
                  [hex(w) for w in fl_words()])

        mem_write(c, cf["mode"], struct.pack(">I", mode_was))
        time.sleep(0.15)

    # --------------------------------------------------- fast loads (1.9, build 11)
    #
    # UYA's fast loads are a code patch now, as RaC1's and RaC2's are: on, two
    # sites of the transition loop become branches into a cave; a planet load
    # forces them behind the checkbox, and the end of the load - the request
    # word down with the mode out of 6, once it has been 6 - puts the
    # checkbox's setting back. The PLANET_LOAD above forced them over empty
    # words, so that load is run and finished first.
    if spec.get("fastload"):
        fl = spec["fastload"]
        site = struct.pack(">I", fl["site_word"])
        draw = struct.pack(">I", fl["draw_word"])

        mem_write(c, fl["mode"], struct.pack(">I", 6))
        time.sleep(0.15)
        check(mem_read_u32(c, fl["site"]) == fl["branch"],
              "%s: the load above runs, in mode 6, with its forced fast loads in" % name,
              hex(mem_read_u32(c, fl["site"])))
        mem_write(c, fl["request"], struct.pack(">I", 0))
        mem_write(c, fl["mode"], struct.pack(">I", 0))
        time.sleep(0.15)
        check(mem_read_u32(c, fl["site"]) == 0 and mem_read_u32(c, fl["draw"]) == 0,
              "%s: and its end took them out of both sites" % name,
              (hex(mem_read_u32(c, fl["site"])), hex(mem_read_u32(c, fl["draw"]))))

        mem_write(c, fl["site"], site)
        mem_write(c, fl["draw"], draw)
        status, _ = c.call(OP_PLANET_LOAD, bytes([fl["planet"], 0]))
        if check(status == ST_OK, "%s: another PLANET_LOAD" % name, status):
            check(mem_read_u32(c, fl["site"]) == fl["branch"] and
                  mem_read_u32(c, fl["draw"]) == fl["draw_branch"],
                  "%s: forces fast loads on, a branch at 0x%X and at 0x%X" %
                  (name, fl["site"], fl["draw"]),
                  (hex(mem_read_u32(c, fl["site"])), hex(mem_read_u32(c, fl["draw"]))))
            check(mem_read_u32(c, fl["cave"]) == fl["cave_first"],
                  "%s: into the cave at 0x%X" % (name, fl["cave"]),
                  hex(mem_read_u32(c, fl["cave"])))
            info = fresh_state(c)
            check(info is not None and not info["toggle_state"] & (1 << fl["toggle"]),
                  "%s: while the checkbox stays off" % name,
                  info["toggle_state"] if info else None)
            time.sleep(0.15)
            mem_write(c, fl["request"], struct.pack(">I", 0))
            time.sleep(0.15)
            check(mem_read_u32(c, fl["site"]) == fl["branch"],
                  "%s: the request word down before the mode was 6 is not the end" % name,
                  hex(mem_read_u32(c, fl["site"])))
            mem_write(c, fl["request"], struct.pack(">I", 1))
            mem_write(c, fl["mode"], struct.pack(">I", 6))
            time.sleep(0.15)
            mem_write(c, fl["request"], struct.pack(">I", 0))
            mem_write(c, fl["mode"], struct.pack(">I", 0))
            time.sleep(0.15)
            check(mem_read_u32(c, fl["site"]) == fl["site_word"] and
                  mem_read_u32(c, fl["draw"]) == fl["draw_word"],
                  "%s: the load's end after mode 6 puts the game's words back" % name,
                  (hex(mem_read_u32(c, fl["site"])), hex(mem_read_u32(c, fl["draw"]))))

        status, _ = c.call(OP_FEATURE_SET, struct.pack(">BI", fl["toggle"], 1))
        if check(status == ST_OK, "%s: the Fast loads toggle turns on" % name, status):
            check(mem_read_u32(c, fl["site"]) == fl["branch"],
                  "%s: and patches the site" % name, hex(mem_read_u32(c, fl["site"])))

        # A load the game starts on its own leaves the checkbox's patch in.
        mem_write(c, fl["request"], struct.pack(">II", 1, fl["planet"]))
        time.sleep(0.15)
        mem_write(c, fl["request"], struct.pack(">I", 0))
        time.sleep(0.15)
        check(mem_read_u32(c, fl["site"]) == fl["branch"],
              "%s: a load the game started ends with it still on" % name,
              hex(mem_read_u32(c, fl["site"])))

        status, _ = c.call(OP_FEATURE_SET, struct.pack(">BI", fl["toggle"], 0))
        check(status == ST_OK and mem_read_u32(c, fl["site"]) == fl["site_word"],
              "%s: and off puts the game's word back" % name,
              (status, hex(mem_read_u32(c, fl["site"]))))

    if spec.get("freeze_health"):
        exercise_freeze_health(c, name, spec["freeze_health"])

    # ------------------------------------------------------ autosplitting
    if udp is not None and spec.get("autosplit"):
        exercise_autosplit(c, udp, name, spec["autosplit"])


# ------------------------------------------------------------------- RPCS3

# The RaC1 fingerprint from src/games/rac1.h and rac1.c, in PS3 byte order: the
# session will not leave BOOTING until it reads these four bytes back.
RAC1_FP_ADDR = 0x0007F558
RAC1_FP = bytes([0x30, 0x64, 0x9C, 0xE0])

# A port of its own, so this can run beside the host smoke above. Free-chosen
# like PORT: the RPCS3 helper is the very program the user may have running.
RPCS3_PORT = free_port()



def wait_savefile_pending_clear(c, timeout=3.0):
    """Polls INFO until no request is outstanding, as a client's save loop does."""
    deadline = time.time() + timeout
    info = savefile_info(c)
    while info is not None and info["pending"] != 0 and time.time() < deadline:
        time.sleep(0.02)
        info = savefile_info(c)
    return info


def smoke_rpcs3_savefile_patch(c, pine, features):
    """
    Build 47, revision 1.15: the savefile helper under RPCS3, once a patch has
    put it in the game.

    RPCS3 applies a patch file as it loads the executable, before it recompiles
    anything, so here the words from SAVEFILE_PATCH go straight into the fake
    emulator's memory, the way RPCS3 writes them and never through qwark, and
    the game is restarted for them to count, as a user restarts it once the
    client has written the patch. One game, RaC1, and two restarts: one with
    the words alone over request bytes that are not zero, the case the patch's
    byte entries exist for, and one with the whole patch. Every restart is a
    second of boot, and the unit tests cover the other three games.
    """
    got = check_savefile_patch(c, "rpcs3")
    if got is None:
        return
    stamp, words, single = got
    check(len(words) > 100, "rpcs3: RaC1's helper is a few hundred bytes of words", len(words))
    check([addr for addr, _ in single] ==
          [RAC1_SF_API_MOD, RAC1_SF_API_LOAD, RAC1_SF_API_SETASIDE],
          "rpcs3: the bytes are api_mod, api_load and api_setaside, in that order",
          [hex(addr) for addr, _ in single])

    # Written into the game that is already running they change nothing: the
    # look is once a session, and a patch only ever goes in at load.
    for addr, value in words:
        pine.poke(addr, value)
    info = savefile_info(c)
    check(info is not None and info["installed"] == 0,
          "rpcs3: words that arrive mid-session do not count until a restart", info)

    # The request bytes as RaC2's executable loaded its own until build 50 moved
    # them out of its code segment: 80 FE A4. The words alone leave them there.
    pine.poke(RAC1_SF_API_MOD, b"\x80\xFE\xA4")
    pine.shutdown_game()
    ok, _ = wait_state(c, SESSION_XMB)
    check(ok, "rpcs3: the game is stopped for the patch to take")
    pine.boot("NPEA00385", "Ratchet & Clank")
    ok, _ = wait_state(c, SESSION_INGAME)
    check(ok, "rpcs3: and comes back with the words applied at load, but not the bytes")
    info = savefile_info(c)
    check(info is not None and info["installed"] == 1,
          "rpcs3: the helper is found: the bytes are no part of the look", info)
    check(info is not None and
          info["pending"] == SAVEFILE_PENDING_SET_ASIDE | SAVEFILE_PENDING_LOAD,
          "rpcs3: and INFO reports a set-aside and a load outstanding from boot", info)
    time.sleep(0.5)
    info = savefile_info(c)
    check(info is not None and
          info["pending"] == SAVEFILE_PENDING_SET_ASIDE | SAVEFILE_PENDING_LOAD,
          "rpcs3: which never clears: the case the byte entries are there for", info)

    # Now the whole patch: the byte entries go in at load over the same bytes.
    pine.shutdown_game()
    ok, _ = wait_state(c, SESSION_XMB)
    check(ok, "rpcs3: the game is stopped again")
    for addr, value in words:
        pine.poke(addr, value)
    for addr, value in single:
        pine.poke(addr, bytes([value]))
    pine.boot("NPEA00385", "Ratchet & Clank")
    ok, _ = wait_state(c, SESSION_INGAME)
    check(ok, "rpcs3: and comes back with the whole patch applied at load")
    # Build 53: a save of the game's own, which RaC1 sets aside and loads alone.
    for addr, data in RAC1_SAVE_WRITES:
        pine.poke(addr, data)
    del pine.write_log[:]

    check(pine.peek(RAC1_SF_API_MOD, 3) == b"\0\0\0",
          "rpcs3: the three request bytes read 0", pine.peek(RAC1_SF_API_MOD, 3).hex())
    info = savefile_info(c)
    check(info is not None and info["installed"] == 1,
          "rpcs3: SAVEFILE_INFO now says the helper is in the game", info)
    check(info is not None and info["pending"] == 0,
          "rpcs3: with nothing outstanding", info)
    check(info is not None and info["running"] == 0,
          "rpcs3: not running while its own byte reads 0", info)

    # The helper's own byte, as it reads once the hook has run a frame.
    pine.poke(RAC1_SF_API_MOD, b"\x01")
    info = savefile_info(c)
    check(info is not None and info["running"] == 1,
          "rpcs3: and running once that byte reads 1", info)

    status, stamp2, _, _, _ = savefile_patch(c)
    check(status == ST_OK and stamp2 == stamp,
          "rpcs3: the same helper after the restart, by the same stamp", stamp2)

    status, _ = c.call(OP_SAVEFILE_CATEGORIES)
    check(status == ST_OK, "rpcs3: the savefile library answers now", status)

    aside = [f for f in features if f["flags"] & FEATURE_FLAG_SAVE_ASIDE]
    load = [f for f in features if f["flags"] & FEATURE_FLAG_LOAD_ASIDE]
    if not (aside and load):
        check(False, "rpcs3: DESCRIBE names the two savefile actions")
        return

    # A set-aside round trip, with the test playing the helper's part.
    status, _ = c.call(OP_FEATURE_TRIGGER, bytes([aside[0]["id"]]))
    check(status == ST_OK, "rpcs3: the SAVE_ASIDE action fires", status)
    check(pine.peek(RAC1_SF_API_SETASIDE, 1) == b"\x01",
          "rpcs3: and raises the set-aside byte in the emulator")
    check(pine.peek(RAC1_SF_API_MOD, 1) == b"\x01",
          "rpcs3: leaving the helper's own byte alone: nothing is zeroed first")
    info = savefile_info(c)
    check(info is not None and info["pending"] == SAVEFILE_PENDING_SET_ASIDE,
          "rpcs3: INFO says the set-aside is outstanding", info)

    pattern = bytes((i * 11 + 5) & 0xFF for i in range(4096))
    pine.poke(RAC1_SF_ASIDE, pattern)
    pine.poke(RAC1_SF_API_SETASIDE, b"\x00")
    info = wait_savefile_pending_clear(c)
    check(info is not None and info["pending"] == 0,
          "rpcs3: the helper clears it and the settle window follows", info)

    status, body = c.call(OP_SAVEFILE_READ, struct.pack(">II", 0, len(pattern)))
    check(status == ST_OK and body == pattern,
          "rpcs3: SAVEFILE_READ hands the set-aside save back", (status, len(body)))

    pine.poke(RAC1_SF_ASIDE, RAC1_SAVE_HEAD)   # build 53: the load hands over a save
    status, _ = c.call(OP_FEATURE_TRIGGER, bytes([load[0]["id"]]))
    check(status == ST_OK, "rpcs3: the LOAD_ASIDE action fires", status)
    check(pine.peek(RAC1_SF_API_LOAD, 1) == b"\x01",
          "rpcs3: and raises the load byte")
    pine.poke(RAC1_SF_API_LOAD, b"\x00")
    info = wait_savefile_pending_clear(c)
    check(info is not None and info["pending"] == 0, "rpcs3: and the load settles", info)

    code = set(addr for addr, _ in words)
    hits = [(hex(a), w) for a, w in pine.write_log
            if any(((a + k) & ~3) in code for k in range(w))]
    check(any(a == RAC1_SF_API_SETASIDE for a, _ in pine.write_log),
          "rpcs3: the emulator's write log saw the request bytes go in")
    check(not hits, "rpcs3: and it saw no cave byte and no hook word written", hits[:4])


# Build 50: RaC1's code switches. Its three flag bytes, from
# src/games/switches_bins.h, and the ids of the toggles they belong to.
RAC1_SW_FLAGS = [0x009058B0, 0x009058B1, 0x009058B2]
RAC1_SW_AMMO_ID = 1
RAC1_SW_AMMO_FLAG = 0x009058B1


def wait_info_flag(c, flag, want, timeout=3.0):
    """Polls HELLO until SessionInfo flags has `flag` set (want) or clear."""
    deadline = time.time() + timeout
    info = None
    while time.time() < deadline:
        status, body = c.call(OP_HELLO, bytes([1]))
        info = parse_session_info(body) if status == ST_OK else None
        if info is not None and bool(info["flags"] & flag) == want:
            return True, info
        time.sleep(0.05)
    return False, info


def smoke_rpcs3_switches(c, pine):
    """
    Build 50, revision 1.17: RaC1's WRITES_CODE toggles under RPCS3, once the
    code switches are in the game.

    RPCS3 applies a patch file as it loads the executable, so the words from
    SWITCH_PATCH go straight into the fake emulator's memory, never through
    qwark, and the game is restarted for them to count. Then a WRITES_CODE
    checkbox is a write of its flag byte: the emulator sees that byte change
    and no code word at all. RaC1 is running and stays running at the end.
    """
    status, stamp, words, single, body = savefile_patch(c, OP_SWITCH_PATCH)
    if not check(status == ST_OK, "rpcs3 switches: SWITCH_PATCH answers", status):
        return
    n = struct.unpack(">H", body[:2])[0]
    check(len(body) == 8 + n * 8 + 4 + len(single) * 8 and body[2:4] == b"\0\0",
          "rpcs3 switches: in SAVEFILE_PATCH's layout", len(body))
    check(stamp == (zlib.crc32(body[8:]) & 0xFFFFFFFF),
          "rpcs3 switches: stamped with the CRC-32 of everything after the stamp", stamp)
    check(n == 44, "rpcs3 switches: RaC1's are 39 trampoline words and 5 sites", n)
    check([addr for addr, _ in single] == RAC1_SW_FLAGS and all(v == 0 for _, v in single),
          "rpcs3 switches: and its three flag bytes, each at 0",
          [(hex(a), v) for a, v in single])

    ok, info = wait_info_flag(c, SESSION_FLAG_CODE_SWITCHES, False)
    check(ok, "rpcs3 switches: without them SessionInfo bit4 is clear",
          hex(info["flags"]) if info else None)
    status, _ = c.call(OP_FEATURE_SET, struct.pack(">BI", RAC1_SW_AMMO_ID, 1))
    check(status == ST_UNSUPPORTED, "rpcs3 switches: and Infinite ammo is refused", status)

    # -------------------------------------------------------- the restart
    pine.shutdown_game()
    ok, _ = wait_state(c, SESSION_XMB)
    check(ok, "rpcs3 switches: the game is stopped for the patch to take")
    for addr, value in words:
        pine.poke(addr, value)
    for addr, value in single:
        pine.poke(addr, bytes([value]))
    del pine.write_log[:]
    pine.boot("NPEA00385", "Ratchet & Clank")
    ok, _ = wait_state(c, SESSION_INGAME)
    check(ok, "rpcs3 switches: and comes back with them applied at load")

    ok, info = wait_info_flag(c, SESSION_FLAG_CODE_SWITCHES, True)
    check(ok, "rpcs3 switches: SessionInfo bit4 CODE_SWITCHES is set",
          hex(info["flags"]) if info else None)
    check(info is not None and info["flags"] & SESSION_FLAG_NO_CODE_PATCHES,
          "rpcs3 switches: beside bit2, which has not changed")

    status, _ = c.call(OP_FEATURE_SET, struct.pack(">BI", RAC1_SW_AMMO_ID, 1))
    check(status == ST_OK, "rpcs3 switches: FEATURE_SET turns Infinite ammo on", status)
    check(pine.peek(RAC1_SW_AMMO_FLAG, 1) == b"\x01",
          "rpcs3 switches: by writing 1 into its flag byte in the emulator",
          pine.peek(RAC1_SW_AMMO_FLAG, 1).hex())
    info = fresh_state(c)
    check(info is not None and info["toggle_state"] & (1 << RAC1_SW_AMMO_ID),
          "rpcs3 switches: and its toggle_state bit is set", info["toggle_state"] if info else None)

    status, _ = c.call(OP_FEATURE_SET, struct.pack(">BI", RAC1_SW_AMMO_ID, 0))
    check(status == ST_OK and pine.peek(RAC1_SW_AMMO_FLAG, 1) == b"\x00",
          "rpcs3 switches: and off again writes 0", status)

    # The byte is the truth: written behind qwark's back, the checkbox follows.
    pine.poke(RAC1_SW_AMMO_FLAG, b"\x01")
    time.sleep(0.4)
    info = fresh_state(c)
    check(info is not None and info["toggle_state"] & (1 << RAC1_SW_AMMO_ID),
          "rpcs3 switches: the checkbox reads the flag byte back", info["toggle_state"] if info else None)
    status, _ = c.call(OP_FEATURE_SET, struct.pack(">BI", RAC1_SW_AMMO_ID, 0))
    check(status == ST_OK, "rpcs3 switches: and turns it off for what follows", status)

    code = set()
    for addr, _ in words:
        code.update(range(addr, addr + 4))
    hits = [(hex(a), w) for a, w in pine.write_log if any((a + k) in code for k in range(w))]
    check(any(a == RAC1_SW_AMMO_FLAG for a, _ in pine.write_log),
          "rpcs3 switches: the emulator's write log saw the flag byte go in")
    check(not hits, "rpcs3 switches: and saw no trampoline byte and no site word written", hits[:4])


# Build 49: a mod uploaded to qwark-rpcs3 and put in the game as an RPCS3 patch.
# Addresses clear of RaC1's savefile helper, its aside buffer and its tables.
RPCS3_MOD_DIR = "rpcs3-smoke"
RPCS3_MOD_FOLDER = "/dev_hdd0/qwark/mods/NPEA00385/" + RPCS3_MOD_DIR
RPCS3_MOD_CAVE = 0x01480000
RPCS3_MOD_WORDS = 0x01400000
# 256 words eight bytes apart, so no two are one read: the look takes a
# second's worth of ticks, long enough to watch it say CHECKING.
RPCS3_MOD_NWORDS = 256
# 22 bytes: five whole words and two tail bytes.
RPCS3_MOD_BIN = bytes((0x40 + i) for i in range(22))


def rpcs3_mod_text():
    lines = ["#- name: RPCS3 smoke", "#- version: 1.0", "#- author: qwark tests",
             "0x%08X: cave.bin" % RPCS3_MOD_CAVE]
    for i in range(RPCS3_MOD_NWORDS):
        lines.append("0x%08X: 0x%08X" % (RPCS3_MOD_WORDS + i * 8, 0x60000000 + i))
    return ("\r\n".join(lines) + "\r\n").encode()


def wait_mod_looked(c, dirname, timeout=10.0):
    """Polls MOD_LIST until the mod is not CHECKING. (row, seen_checking)."""
    deadline = time.time() + timeout
    seen = False
    row = mod_row(c, dirname)
    while row is not None and row["flags"] & MOD_FLAG_CHECKING and time.time() < deadline:
        seen = True
        time.sleep(0.02)
        row = mod_row(c, dirname)
    return row, seen


def smoke_rpcs3_mods(c, pine):
    """
    Build 49, revision 1.16: a mod under RPCS3. The client uploads it with the
    file ops, as it would to a console - the files, qwark.sum, MOD_RESCAN - and
    then writes it from MOD_PATCH into RPCS3's patch file, which RPCS3 applies
    as it loads the game. Here the rows and bytes go straight into the fake
    emulator's memory, never through qwark, and the game is restarted for them
    to count. RaC1 is running and stays running at the end.
    """
    text = rpcs3_mod_text()
    status, _ = c.call(OP_DIR_CREATE, RPCS3_MOD_FOLDER.encode())
    check(status == ST_OK, "rpcs3 mods: the mod's folder is created under --root", status)
    ok = write_console_file(c, RPCS3_MOD_FOLDER + "/patch.txt", text)
    ok = write_console_file(c, RPCS3_MOD_FOLDER + "/cave.bin", RPCS3_MOD_BIN) and ok
    crc = zlib.crc32(text + RPCS3_MOD_BIN) & 0xFFFFFFFF
    ok = write_console_file(c, RPCS3_MOD_FOLDER + "/qwark.sum", ("%08x" % crc).encode()) and ok
    check(ok, "rpcs3 mods: patch.txt, cave.bin and qwark.sum are uploaded")
    status, _ = c.call(OP_MOD_RESCAN)
    check(status == ST_OK, "rpcs3 mods: MOD_RESCAN", status)

    row = mod_row(c, RPCS3_MOD_DIR)
    if not check(row is not None, "rpcs3 mods: MOD_LIST has the uploaded mod"):
        return
    check(row["hash"] == crc, "rpcs3 mods: with the hash the upload wrote", "%08x" % row["hash"])
    row, _ = wait_mod_looked(c, RPCS3_MOD_DIR)
    check(row is not None and row["flags"] & (MOD_FLAG_LOADED | MOD_FLAG_CHECKING) == 0,
          "rpcs3 mods: once looked for, it is not in the game", row)

    status, _ = c.call(OP_MOD_LOAD, fixed32(RPCS3_MOD_DIR))
    check(status == ST_UNSUPPORTED, "rpcs3 mods: MOD_LOAD is still UNSUPPORTED here", status)

    # ----------------------------------------------------------- MOD_PATCH
    status, stamp, rows, single, body = mod_patch(c, row["index"])
    if not check(status == ST_OK, "rpcs3 mods: MOD_PATCH answers", status):
        return
    cave_rows = [(RPCS3_MOD_CAVE + k, RPCS3_MOD_BIN[k:k + 4]) for k in range(0, 20, 4)]
    word_rows = [(RPCS3_MOD_WORDS + i * 8, struct.pack(">I", 0x60000000 + i))
                 for i in range(RPCS3_MOD_NWORDS)]
    check(rows == cave_rows + word_rows,
          "rpcs3 mods: the cave's five words first, then the 256 words in patch.txt order",
          len(rows))
    check(single == [(RPCS3_MOD_CAVE + 20, RPCS3_MOD_BIN[20]), (RPCS3_MOD_CAVE + 21, RPCS3_MOD_BIN[21])],
          "rpcs3 mods: and the cave's two tail bytes after them", single)
    check(len(body) == 8 + len(rows) * 8 + 4 + len(single) * 8 and body[2:4] == b"\0\1",
          "rpcs3 mods: in SAVEFILE_PATCH's layout, on one page (revision 1.19)", len(body))
    check(stamp == (zlib.crc32(body[8:]) & 0xFFFFFFFF),
          "rpcs3 mods: stamped with the CRC-32 of everything after the stamp", stamp)

    # Written into the running game they change nothing until a restart.
    for addr, value in rows:
        pine.poke(addr, value)
    for addr, value in single:
        pine.poke(addr, bytes([value]))
    time.sleep(0.3)
    row = mod_row(c, RPCS3_MOD_DIR)
    check(row is not None and row["flags"] & MOD_FLAG_LOADED == 0,
          "rpcs3 mods: words that arrive mid-session do not count until a restart", row)

    # -------------------------------------------------------- the restart
    pine.shutdown_game()
    ok, _ = wait_state(c, SESSION_XMB)
    check(ok, "rpcs3 mods: the game is stopped for the patch to take")
    row = mod_row(c, RPCS3_MOD_DIR)
    check(row is not None and row["flags"] & (MOD_FLAG_LOADED | MOD_FLAG_CHECKING) == 0,
          "rpcs3 mods: at the XMB the mod is neither LOADED nor CHECKING", row)

    del pine.write_log[:]
    pine.boot("NPEA00385", "Ratchet & Clank")
    ok, _ = wait_state(c, SESSION_INGAME)
    check(ok, "rpcs3 mods: and comes back with the mod applied at load")
    row, seen = wait_mod_looked(c, RPCS3_MOD_DIR)
    check(seen, "rpcs3 mods: MOD_LIST says CHECKING while qwark looks")
    check(row is not None and row["flags"] == MOD_FLAG_LOADED,
          "rpcs3 mods: and then LOADED, the mod found in game memory", row)

    status, _ = c.call(OP_MOD_RESCAN)
    row = mod_row(c, RPCS3_MOD_DIR)
    check(status == ST_OK and row is not None and row["flags"] & MOD_FLAG_CHECKING,
          "rpcs3 mods: a rescan looks again", row)
    row, _ = wait_mod_looked(c, RPCS3_MOD_DIR)
    check(row is not None and row["flags"] == MOD_FLAG_LOADED,
          "rpcs3 mods: and finds it again", row)

    mine = set()
    for addr, _ in rows:
        mine.update(range(addr, addr + 4))
    mine.update(addr for addr, _ in single)
    hits = [(hex(a), w) for a, w in pine.write_log if any((a + k) in mine for k in range(w))]
    check(not hits, "rpcs3 mods: and qwark wrote none of it into the emulator", hits[:4])


# Revision 1.19: the big-cave fixture, a 10240-byte cave and two words, is 2562
# rows: more than the 2046 one frame holds, so MOD_PATCH answers it in two pages.
PAGES_MOD_DIR = "big-cave"
PAGES_MOD_SRC = os.path.join(HERE, "fixtures", "mods", "NPEA00385", PAGES_MOD_DIR)
PAGES_MOD_FOLDER = "/dev_hdd0/qwark/mods/NPEA00385/" + PAGES_MOD_DIR
MOD_PATCH_PAGE_ROWS = (QWARK_MAX_PAYLOAD - 8 - 4) // 8


def unpaged_mod_layout(folder):
    """
    What one unbounded MOD_PATCH reply would hold for the mod in `folder`, from
    its files: every cave in patch.txt order as whole words, then every word,
    and the caves' tail bytes in address order. (rows, single, stamp), the stamp
    being the CRC-32 of the rows, u16 nb, u16 pad and the byte rows.
    """
    with open(os.path.join(folder, "patch.txt"), "rb") as f:
        text = f.read().decode()
    caves, words = [], []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or ":" not in line:
            continue
        addr, value = (s.strip() for s in line.split(":", 1))
        if not addr.lower().startswith("0x"):
            continue
        if value.lower().startswith("0x"):
            words.append((int(addr, 16), struct.pack(">I", int(value, 16))))
        else:
            with open(os.path.join(folder, value), "rb") as f:
                caves.append((int(addr, 16), f.read()))
    rows, single = [], []
    for addr, data in caves:
        whole = len(data) - len(data) % 4
        rows += [(addr + k, data[k:k + 4]) for k in range(0, whole, 4)]
        single += [(addr + k, data[k]) for k in range(whole, len(data))]
    rows += words
    single.sort()
    flat = b"".join(struct.pack(">I", a) + v for a, v in rows)
    flat += struct.pack(">HH", len(single), 0)
    flat += b"".join(struct.pack(">IB3x", a, v) for a, v in single)
    return rows, single, zlib.crc32(flat) & 0xFFFFFFFF


def smoke_rpcs3_mod_pages(c, pine):
    """
    Build 52, revision 1.19: a mod bigger than one frame under RPCS3. The
    big-cave fixture is uploaded as a client uploads any mod, fetched a page at
    a time - page 0 with the request a client before 1.19 sends - and put back
    together; written into the fake emulator and the game restarted, it is what
    qwark's look calls LOADED. RaC1 is running and stays running at the end.
    """
    files = ["patch.txt", "rack.bin"]
    status, _ = c.call(OP_DIR_CREATE, PAGES_MOD_FOLDER.encode())
    check(status == ST_OK, "rpcs3 mod pages: big-cave's folder is created", status)
    ok = True
    for name in files:
        with open(os.path.join(PAGES_MOD_SRC, name), "rb") as f:
            ok = write_console_file(c, PAGES_MOD_FOLDER + "/" + name, f.read()) and ok
    check(ok, "rpcs3 mod pages: the fixture's patch.txt and 10240-byte rack.bin are uploaded")
    status, _ = c.call(OP_MOD_RESCAN)
    check(status == ST_OK, "rpcs3 mod pages: MOD_RESCAN", status)
    row = mod_row(c, PAGES_MOD_DIR)
    if not check(row is not None, "rpcs3 mod pages: MOD_LIST has big-cave"):
        return
    index = row["index"]
    row, _ = wait_mod_looked(c, PAGES_MOD_DIR)
    check(row is not None and row["flags"] & (MOD_FLAG_LOADED | MOD_FLAG_CHECKING) == 0,
          "rpcs3 mod pages: once looked for, it is not in the game", row)

    want_rows, want_single, want_stamp = unpaged_mod_layout(PAGES_MOD_SRC)
    check(len(want_rows) + len(want_single) > MOD_PATCH_PAGE_ROWS,
          "rpcs3 mod pages: its %d rows are more than one frame's %d"
          % (len(want_rows) + len(want_single), MOD_PATCH_PAGE_ROWS))

    # ------------------------------------------------- page by page
    status, pages, stamp, rows0, single0, body0 = mod_patch_page(
        c, index, 0, request=bytes([index, 0, 0, 0]))
    if not check(status == ST_OK, "rpcs3 mod pages: page 0, asked the way a 1.18 client asks", status):
        return
    check(pages == 2, "rpcs3 mod pages: two pages", pages)
    check(len(rows0) == MOD_PATCH_PAGE_ROWS and single0 == [] and len(body0) <= QWARK_MAX_PAYLOAD,
          "rpcs3 mod pages: page 0 is a full frame of word rows and no bytes",
          (len(rows0), len(single0), len(body0)))
    status, d, s, again, _, _ = mod_patch_page(c, index, 0)
    check(status == ST_OK and (d, s) == (pages, stamp) and again == rows0,
          "rpcs3 mod pages: u16 page 0 is the same page", status)

    got_rows, got_single, ok = list(rows0), [], True
    for page in range(1, pages):
        status, d, s, rows, single, body = mod_patch_page(c, index, page)
        if not check(status == ST_OK, "rpcs3 mod pages: page %d answers" % page, status):
            return
        check(d == pages and s == stamp,
              "rpcs3 mod pages: page %d has the same pages and stamp" % page, (d, s))
        check(len(body) == 8 + len(rows) * 8 + 4 + len(single) * 8 and len(body) <= QWARK_MAX_PAYLOAD,
              "rpcs3 mod pages: page %d parses to its length, inside a frame" % page, len(body))
        if page < pages - 1 and single:
            ok = False
        got_rows += rows
        got_single = single
    check(ok, "rpcs3 mod pages: byte rows only on the last page")

    status, _ = c.call(OP_MOD_PATCH, struct.pack(">BBH", index, 0, pages))
    check(status == ST_BAD_ARG, "rpcs3 mod pages: page %d, past the last, is BAD_ARG" % pages, status)

    check(got_rows == want_rows,
          "rpcs3 mod pages: the pages joined are its 2560 cave words, then its 2 words",
          (len(got_rows), len(want_rows)))
    check(got_single == want_single, "rpcs3 mod pages: and its byte rows", got_single)
    check(stamp == want_stamp,
          "rpcs3 mod pages: the stamp is the CRC-32 of the whole unpaged reply after it",
          "%08x vs %08x" % (stamp or 0, want_stamp))
    addrs = [a for a, _ in got_rows] + [a for a, _ in got_single]
    check(len(addrs) == len(set(addrs)), "rpcs3 mod pages: no address appears twice")

    # ------------------------- written in at load, it is what the look finds
    for addr, value in got_rows:
        pine.poke(addr, value)
    for addr, value in got_single:
        pine.poke(addr, bytes([value]))
    pine.shutdown_game()
    ok, _ = wait_state(c, SESSION_XMB)
    check(ok, "rpcs3 mod pages: the game is stopped for the patch to take")
    del pine.write_log[:]
    pine.boot("NPEA00385", "Ratchet & Clank")
    ok, _ = wait_state(c, SESSION_INGAME)
    check(ok, "rpcs3 mod pages: and comes back with both pages applied at load")
    row, _ = wait_mod_looked(c, PAGES_MOD_DIR)
    check(row is not None and row["flags"] == MOD_FLAG_LOADED,
          "rpcs3 mod pages: and the reassembled mod is LOADED, every cave byte and word of it", row)

    mine = set()
    for addr, _ in got_rows:
        mine.update(range(addr, addr + 4))
    hits = [(hex(a), w) for a, w in pine.write_log if any((a + k) in mine for k in range(w))]
    check(not hits, "rpcs3 mod pages: and qwark wrote none of it into the emulator", hits[:4])


def smoke_rpcs3(exe):
    """qwark-rpcs3.exe against the fake PINE server: flags, boot, DESCRIBE."""
    if not os.path.exists(exe):
        check(False, "%s exists, run build-host.sh first" % os.path.basename(exe))
        return

    root = os.path.join(HERE, "qwark-rpcs3-root")
    shutil.rmtree(root, ignore_errors=True)

    pine = FakePine()
    pine.poke(RAC1_FP_ADDR, RAC1_FP)

    # --bind spelled out here, so the flag itself is exercised; 127.0.0.1 is
    # also what it defaults to, and the only address anything here ever binds.
    sim = Sim(exe, ["--pine-port", str(pine.port),
                    "--port", str(RPCS3_PORT),
                    "--bind", HOST,
                    "--root", root])
    sock = None

    try:
        sock = connect(port=RPCS3_PORT)
        c = Client(sock)

        # ------------------------------------------------------------ hello
        status, body = c.call(OP_HELLO, bytes([1]))
        info = parse_session_info(body) if status == ST_OK else None
        check(status == ST_OK and len(body) == SESSION_INFO_SIZE,
              "rpcs3: HELLO returns a 164-byte SessionInfo", (status, len(body)))
        check(info and info["build"] == QWARK_BUILD,
              "rpcs3: it reports build %d" % QWARK_BUILD,
              info["build"] if info else None)

        # The banner says where it bound, and we are talking to it there.
        want = "client on %s:%d" % (HOST, RPCS3_PORT)
        line = sim_line(sim, "client on ")
        check(line is not None and want in line,
              "rpcs3: --bind %s is honoured and announced" % HOST, line)

        # Two helpers cannot share a port any more. The host builds ask for no
        # SO_REUSEADDR, so a second one started on a port that is already being
        # served loses the bind and says so, rather than quietly taking the
        # listener over - which is how a suite run used to end up testing the
        # qwark-rpcs3.exe behind the user's own client instead of the build under
        # test. Its own root and a PINE port nothing listens on keep it clear of
        # the helper we are actually testing.
        rival_root = os.path.join(HERE, "qwark-rpcs3-rival-root")
        shutil.rmtree(rival_root, ignore_errors=True)
        rival = Sim(exe, ["--pine-port", str(free_port()),
                          "--port", str(RPCS3_PORT),
                          "--bind", HOST,
                          "--root", rival_root])
        try:
            said = sim_line(rival, "cannot listen", timeout=5.0)
            check(said is not None,
                  "rpcs3: a second helper on the same port loses the bind and says so",
                  rival.lines[-3:])
        finally:
            rival.stop()
            shutil.rmtree(rival_root, ignore_errors=True)

        status, body = c.call(OP_HELLO, bytes([1]))
        info = parse_session_info(body) if status == ST_OK else None
        check(status == ST_OK,
              "rpcs3: and the helper under test still owns the port", status)

        check(info and (info["flags"] & SESSION_FLAG_EMULATOR) != 0,
              "rpcs3: flags bit1 EMULATOR is set",
              hex(info["flags"]) if info else None)
        check(info and (info["flags"] & SESSION_FLAG_NO_CODE_PATCHES) != 0,
              "rpcs3: flags bit2 NO_CODE_PATCHES is set",
              hex(info["flags"]) if info else None)
        check(info and info["state"] == SESSION_XMB,
              "rpcs3: with no game up the session is in XMB",
              info["state"] if info else None)

        # ------------------------------------------------------------- boot
        # The fake emulator answers Running with a title id, and serves the
        # fingerprint, which is everything the session machine needs.
        pine.boot("NPEA00385", "Ratchet & Clank")
        ok, info = wait_state(c, SESSION_INGAME)
        check(ok, "rpcs3: the session reaches INGAME once PINE says Running", info)
        check(info and info["title"] == "NPEA00385",
              "rpcs3: the title id came from MsgID", info)
        check(info and info["game"] == 1,
              "rpcs3: the fingerprint identified RaC1", info)
        check(info and (info["flags"] & SESSION_FLAG_NO_CODE_PATCHES) != 0,
              "rpcs3: the platform flags survive into the game session")

        # --------------------------------------------------------- describe
        status, body = c.call(OP_DESCRIBE)
        code_rows = []
        if check(status == ST_OK, "rpcs3: DESCRIBE answers OK", status):
            game, groups, readouts, features, consumed = parse_describe(body)
            check(consumed == len(body), "rpcs3: DESCRIBE parses exactly",
                  (consumed, len(body)))
            check(game == 1, "rpcs3: DESCRIBE says RaC1", game)
            check(len(features) > 10, "rpcs3: it lists the RaC1 features",
                  len(features))
            code_rows = [f for f in features
                         if f["flags"] & FEATURE_FLAG_WRITES_CODE]
            check(len(code_rows) > 0,
                  "rpcs3: DESCRIBE still lists the WRITES_CODE rows, unchanged",
                  len(code_rows))

        # ------------------------------------------------ what is refused
        if code_rows:
            row = code_rows[0]
            status, _ = c.call(OP_FEATURE_SET,
                               struct.pack(">BI", row["id"], 1))
            check(status == ST_UNSUPPORTED,
                  "rpcs3: FEATURE_SET on '%s' is UNSUPPORTED" % row["label"],
                  status)

        status, _ = c.call(OP_PATCH_APPLY,
                           struct.pack(">HH", 1, 0) +
                           struct.pack(">II", 0x00500000, 0x60000000))
        check(status == ST_UNSUPPORTED, "rpcs3: PATCH_APPLY is UNSUPPORTED", status)

        # The savefile helper is a code cave and a branch into it, so qwark
        # cannot put it here (protocol 1.9). Build 47: INFO answers anyway, to
        # say whether an RPCS3 patch put it in at load - and none has yet.
        info = savefile_info(c)
        check(info is not None, "rpcs3: SAVEFILE_INFO answers (build 47)")
        check(info is not None and info["supported"] == 1 and info["installed"] == 0,
              "rpcs3: the game is supported and the helper is not in it", info)
        status, _ = c.call(OP_SAVEFILE_READ, struct.pack(">II", 0, 16))
        check(status == ST_UNSUPPORTED, "rpcs3: SAVEFILE_READ is UNSUPPORTED", status)
        status, _ = c.call(OP_SAVEFILE_WRITE, struct.pack(">I", 0) + b"\x01\x02\x03\x04")
        check(status == ST_UNSUPPORTED, "rpcs3: SAVEFILE_WRITE is UNSUPPORTED", status)

        # Revision 1.10: the library on the console is the same helper by
        # another road, so all five of its ops go the same way.
        status, _ = c.call(OP_SAVEFILE_CATEGORIES)
        check(status == ST_UNSUPPORTED, "rpcs3: SAVEFILE_CATEGORIES is UNSUPPORTED", status)
        status, _ = c.call(OP_SAVEFILE_LIST, fixed32("misc"))
        check(status == ST_UNSUPPORTED, "rpcs3: SAVEFILE_LIST is UNSUPPORTED", status)
        status, _ = c.call(OP_SAVEFILE_STORE, fixed32("misc") + fixed32("a.sav"))
        check(status == ST_UNSUPPORTED, "rpcs3: SAVEFILE_STORE is UNSUPPORTED", status)
        status, _ = c.call(OP_SAVEFILE_RESTORE, fixed32("misc") + fixed32("a.sav"))
        check(status == ST_UNSUPPORTED, "rpcs3: SAVEFILE_RESTORE is UNSUPPORTED", status)
        status, _ = c.call(OP_SAVEFILE_CATEGORY,
                           bytes([SAVEFILE_CATEGORY_CREATE]) + fixed32("misc"))
        check(status == ST_UNSUPPORTED, "rpcs3: SAVEFILE_CATEGORY is UNSUPPORTED", status)

        # FILE_RENAME is not a code patch and answers here as it does anywhere.
        rename_payload = (struct.pack(">H", len("/dev_hdd0/qwark/nothing"))
                          + b"/dev_hdd0/qwark/nothing" + b"/dev_hdd0/qwark/nothing2")
        status, _ = c.call(OP_FILE_RENAME, rename_payload)
        check(status == ST_NOT_FOUND, "rpcs3: FILE_RENAME still answers", status)

        aside = [f for f in features if f["flags"] & FEATURE_FLAG_SAVE_ASIDE]
        if aside:
            status, _ = c.call(OP_FEATURE_TRIGGER, bytes([aside[0]["id"]]))
            check(status == ST_UNSUPPORTED,
                  "rpcs3: and so is the SAVE_ASIDE action", status)

        # --------------------------------------------- what still works
        status, _ = c.call(OP_MEM_WRITE,
                           struct.pack(">I", 0x00800000) + b"\xDE\xAD\xBE\xEF")
        check(status == ST_OK, "rpcs3: MEM_WRITE still works", status)
        check(pine.peek(0x00800000, 4) == b"\xDE\xAD\xBE\xEF",
              "rpcs3: and the bytes reached the emulator in PS3 order",
              pine.peek(0x00800000, 4).hex())

        status, body = c.call(OP_MEM_READ, struct.pack(">II", 0x00800000, 4))
        check(status == ST_OK and body == b"\xDE\xAD\xBE\xEF",
              "rpcs3: MEM_READ hands the same bytes back",
              (status, body.hex() if body else None))

        # A read of the fingerprint proves the byte order end to end: the
        # client sees the instruction word, not its reverse.
        status, body = c.call(OP_MEM_READ, struct.pack(">II", RAC1_FP_ADDR, 4))
        check(status == ST_OK and body == RAC1_FP,
              "rpcs3: the RaC1 fingerprint reads back as 30 64 9c e0",
              body.hex() if body else None)

        # ------------------------------------ the helper by an RPCS3 patch
        smoke_rpcs3_savefile_patch(c, pine, features)

        # ------------------------------------ a mod by an RPCS3 patch
        smoke_rpcs3_mods(c, pine)

        # ------------------------- a mod past one frame, a page at a time
        smoke_rpcs3_mod_pages(c, pine)

        # ------------------------------ the code switches by an RPCS3 patch
        smoke_rpcs3_switches(c, pine)

        # ---------------------------------------------------------- paused
        # Pausing the emulator leaves the game, its memory and its title id
        # exactly where they were, so the session must not end.
        pine.status = STATUS_PAUSED
        info = fresh_state(c, settle=0.6)
        check(info is not None and info["state"] == SESSION_INGAME,
              "rpcs3: pausing the emulator does not end the session", info)
        pine.status = STATUS_RUNNING

        # ------------------------------------------------------ the game goes
        pine.shutdown_game()
        ok, info = wait_state(c, SESSION_XMB)
        check(ok, "rpcs3: stopping the emulated game returns the session to XMB",
              info)

        # ---------------------------------------------------- planet loads
        smoke_rpcs3_planet_loads(c, pine)

        # ------------------- RaC2 with an earlier build's code switches
        smoke_rpcs3_rac2_old_switches(c, pine)

        # --------------- UYA's fast loads, a code switch beside Freeze ammo's
        smoke_rpcs3_rac3_switches(c, pine)

        sock.close()
        sock = None

    except Exception as exc:  # noqa: BLE001
        check(False, "rpcs3: the smoke run completed without an exception",
              repr(exc))
    finally:
        if sock is not None:
            sock.close()
        sim.stop()
        pine.stop()


# RaC2's and Deadlocked's fingerprints (src/games/rac2.c, rac4.c) and the words
# their planet loads touch (rac2.h, rac4.h). RaC2's fingerprint site is also its
# fast-load patch site.
RAC2_FP_ADDR = 0x00BEA8A0
RAC2_FP = bytes([0x4B, 0xFF, 0xEA, 0x69])
RAC2_LEVEL_FLAGS = 0x015625B0
RAC2_LOAD_PLANET = 0x0156B050
RAC4_FP_ADDR = 0x001D7D18
RAC4_FP = bytes([0x41, 0x82, 0x00, 0x0C])
# Build 53: the words Deadlocked's fast loads patch, and the game's own.
RAC4_FASTLOAD_ORIG = ([(0x002A8AF4, 0x48120261), (0x002A8AE8, 0x38600000),
                       (0x002A8AEC, 0x80846DD0)] + RAC4_FL_MOVIE_GAME +
                      [(0x002A8C50, 0x4E9E0421), (0x002A8C5C, 0x607B0000),
                       (0x002A8CC8, 0x38600001)])
RAC4_LOAD_PLANET2 = 0x00B36DCC
RAC4_TARGET_PLANET = 0x00B36DD0


def smoke_rpcs3_planet_loads(c, pine):
    """RaC2 and Deadlocked force fast loads, an instruction patch, before a
    planet load. RPCS3 refuses the patch, so the load goes out without it:
    the request and the level-flag reset reach the emulator and the patch
    sites keep their instructions. Starts and ends at XMB."""
    # ------------------------------------------------------------- RaC2
    pine.poke(RAC2_FP_ADDR, RAC2_FP)
    pine.boot("NPEA00386", "Ratchet & Clank: Going Commando")
    ok, info = wait_state(c, SESSION_INGAME)
    check(ok and info["game"] == 2, "rpcs3: RaC2 reaches INGAME", info)

    flag = RAC2_LEVEL_FLAGS + 5 * 0x10 + 2
    pine.poke(flag, b"\xAA")
    status, _ = c.call(OP_PLANET_LOAD, bytes([5, PLANET_FLAG_RESET_LEVELFLAGS]))
    check(status == ST_OK, "rpcs3: RaC2's PLANET_LOAD goes ahead without fast loads",
          status)
    check(pine.peek(RAC2_LOAD_PLANET, 8) == struct.pack(">II", 1, 5),
          "rpcs3: the request and the planet reached the emulator",
          pine.peek(RAC2_LOAD_PLANET, 8).hex())
    check(pine.peek(flag, 1) == b"\x00",
          "rpcs3: and the level flags were zeroed", pine.peek(flag, 1).hex())
    check(pine.peek(RAC2_FP_ADDR, 4) == RAC2_FP,
          "rpcs3: the fast-load site kept its instruction",
          pine.peek(RAC2_FP_ADDR, 4).hex())

    pine.shutdown_game()
    ok, info = wait_state(c, SESSION_XMB)
    check(ok, "rpcs3: RaC2 stops", info)

    # ------------------------------------------------------- Deadlocked
    pine.poke(RAC4_FP_ADDR, RAC4_FP)
    for addr, word in RAC4_FASTLOAD_ORIG:
        pine.poke(addr, struct.pack(">I", word))
    pine.boot("NPEA00423", "Ratchet: Deadlocked")
    ok, info = wait_state(c, SESSION_INGAME)
    check(ok and info["game"] == 4, "rpcs3: Deadlocked reaches INGAME", info)

    status, _ = c.call(OP_PLANET_LOAD, bytes([4, 0]))
    check(status == ST_OK,
          "rpcs3: Deadlocked's PLANET_LOAD goes ahead without fast loads", status)
    check(pine.peek(RAC4_TARGET_PLANET, 4) == struct.pack(">I", 4),
          "rpcs3: the target planet reached the emulator",
          pine.peek(RAC4_TARGET_PLANET, 4).hex())
    check(pine.peek(RAC4_LOAD_PLANET2, 4) == struct.pack(">I", 1),
          "rpcs3: and so did the load request",
          pine.peek(RAC4_LOAD_PLANET2, 4).hex())
    words = [struct.unpack(">I", pine.peek(a, 4))[0] for a, _w in RAC4_FASTLOAD_ORIG]
    check(words == [w for _a, w in RAC4_FASTLOAD_ORIG],
          "rpcs3: every fast-load word kept its instruction",
          [hex(w) for w in words])

    pine.shutdown_game()
    ok, info = wait_state(c, SESSION_XMB)
    check(ok, "rpcs3: Deadlocked stops", info)


# Build 53: RaC2's code switches moved off 0x233200, inside a routine planet Joba
# runs every frame, to 0x1141400. These are the rows the old placement's
# SWITCH_PATCH handed out (builds 50 to 52), which an RPCS3 patch file written
# then still holds; the branch at 0xBEA8A0 is also RaC2's fingerprint site.
RAC2_SW_OLD_ROWS = [
    (0x00233200, 0x3C600131), (0x00233204, 0x88637720), (0x00233208, 0x2C030000),
    (0x0023320C, 0x40820008), (0x00233210, 0x489B60F9), (0x00233214, 0x489B7690),
    (0x00233220, 0x3CC00131), (0x00233224, 0x88C67721), (0x00233228, 0x2C060000),
    (0x0023322C, 0x40820008), (0x00233230, 0x7C64292E), (0x00233234, 0x488FDA4C),
    (0x00B30C7C, 0x4B7025A4), (0x00BEA8A0, 0x4B648960),
]
RAC2_SW_FLAGS = [0x01317720, 0x01317721]
RAC2_SW_AMMO_ID = 1
RAC2_SW_AMMO_FLAG = 0x01317721


def smoke_rpcs3_rac2_old_switches(c, pine):
    """
    Build 53: a RaC2 whose RPCS3 patch still carries the switches of an earlier
    build. The old branch at the fingerprint site must still say RaC2, and the
    client must be able to tell the switches are from another build - bit2 set
    and bit4 clear, and a SWITCH_PATCH stamp that is not the one it kept beside
    the old patch - so it offers to write the patch again. Then the new patch
    goes in and counts. Starts and ends at XMB.
    """
    old_body = b"".join(struct.pack(">II", a, v) for a, v in RAC2_SW_OLD_ROWS)
    old_body += struct.pack(">HH", len(RAC2_SW_FLAGS), 0)
    old_body += b"".join(struct.pack(">I", a) + b"\0\0\0\0" for a in RAC2_SW_FLAGS)
    old_stamp = zlib.crc32(old_body) & 0xFFFFFFFF
    check(old_stamp == 0x4B325BE2,
          "rpcs3 rac2 switches: the old patch's stamp is the one builds 50 to 52 handed out",
          hex(old_stamp))

    for addr, value in RAC2_SW_OLD_ROWS:
        pine.poke(addr, struct.pack(">I", value))
    for addr in RAC2_SW_FLAGS:
        pine.poke(addr, b"\x00")
    pine.boot("NPEA00386", "Ratchet & Clank: Going Commando")
    ok, info = wait_state(c, SESSION_INGAME)
    check(ok and info["game"] == 2,
          "rpcs3 rac2 switches: RaC2 with an earlier build's switch patch reaches INGAME as RaC2",
          info)

    ok, info = wait_info_flag(c, SESSION_FLAG_CODE_SWITCHES, False)
    check(ok and info["flags"] & SESSION_FLAG_NO_CODE_PATCHES,
          "rpcs3 rac2 switches: bit2 set and bit4 clear, so the client offers the patch",
          hex(info["flags"]) if info else None)

    status, stamp, words, single, body = savefile_patch(c, OP_SWITCH_PATCH)
    check(status == ST_OK and stamp != old_stamp,
          "rpcs3 rac2 switches: SWITCH_PATCH's stamp is not the old patch's: another build",
          (status, hex(stamp) if stamp is not None else None))
    rows = [(a, struct.unpack(">I", v)[0]) for a, v in (words or [])]
    # Twenty-nine trampoline words, the crash guards' with them, and four sites.
    check(len(rows) == 33 and all(0x01141400 <= a < 0x011414FC for a, _ in rows[:29]),
          "rpcs3 rac2 switches: its twenty-nine trampoline words are at 0x1141400 on",
          [hex(a) for a, _ in rows[:29]])
    check(rows[29:] == [(0x00086960, 0x490BAAE0), (0x00B30C7C, 0x486107A4),
                        (0x00BDF30C, 0x48562164), (0x00BEA8A0, 0x48556B60)],
          "rpcs3 rac2 switches: and the sites branch there",
          [(hex(a), hex(v)) for a, v in rows[29:]])
    status, _ = c.call(OP_FEATURE_SET, struct.pack(">BI", RAC2_SW_AMMO_ID, 1))
    check(status == ST_UNSUPPORTED,
          "rpcs3 rac2 switches: Infinite ammo is refused until then", status)

    # -------------------------------------- the client rewrites the patch
    pine.shutdown_game()
    ok, _ = wait_state(c, SESSION_XMB)
    check(ok, "rpcs3 rac2 switches: the game is stopped for the new patch")
    for addr, _ in RAC2_SW_OLD_ROWS:
        pine.poke(addr, b"\0\0\0\0")
    pine.poke(RAC2_FP_ADDR, RAC2_FP)
    for addr, value in words:
        pine.poke(addr, value)
    for addr, value in single:
        pine.poke(addr, bytes([value]))
    pine.boot("NPEA00386", "Ratchet & Clank: Going Commando")
    ok, info = wait_state(c, SESSION_INGAME)
    check(ok and info["game"] == 2, "rpcs3 rac2 switches: RaC2 comes back with the new patch", info)
    ok, info = wait_info_flag(c, SESSION_FLAG_CODE_SWITCHES, True)
    check(ok, "rpcs3 rac2 switches: and bit4 CODE_SWITCHES is set",
          hex(info["flags"]) if info else None)

    status, _ = c.call(OP_FEATURE_SET, struct.pack(">BI", RAC2_SW_AMMO_ID, 1))
    check(status == ST_OK and pine.peek(RAC2_SW_AMMO_FLAG, 1) == b"\x01",
          "rpcs3 rac2 switches: Infinite ammo goes on through its flag byte", status)
    status, _ = c.call(OP_FEATURE_SET, struct.pack(">BI", RAC2_SW_AMMO_ID, 0))
    check(status == ST_OK and pine.peek(RAC2_SW_AMMO_FLAG, 1) == b"\x00",
          "rpcs3 rac2 switches: and off", status)

    pine.shutdown_game()
    ok, info = wait_state(c, SESSION_XMB)
    check(ok, "rpcs3 rac2 switches: RaC2 stops", info)
    for addr, _ in words:
        pine.poke(addr, b"\0\0\0\0")
    pine.poke(RAC2_FP_ADDR, RAC2_FP)


# UYA's fast loads were two data words that worked under RPCS3 with nothing
# installed; they are a code patch now, so under RPCS3 they are a code switch
# beside Freeze ammo's (src/games/switches/sw_rac3.s). These are the rows the
# Freeze-ammo-only SWITCH_PATCH handed out (builds 50 to 53), which an RPCS3 patch
# file written then still holds; 0x182A88 is also RaC3's fingerprint site, and
# its branch did not change.
RAC3_FP_ADDR = 0x00182A88
RAC3_FP = bytes([0x7C, 0x85, 0x31, 0x2E])
RAC3_SW_OLD_ROWS = [
    (0x00980F00, 0x3CE000DA), (0x00980F04, 0x88E7FF10), (0x00980F08, 0x2C070000),
    (0x00980F0C, 0x40820008), (0x00980F10, 0x7C85312E), (0x00980F14, 0x4B801B78),
    (0x00182A88, 0x487FE478),
]
RAC3_SW_OLD_FLAGS = [0x00D9FF10]
RAC3_SW_FREEZE_ID = 0
RAC3_SW_FAST_ID = 38
RAC3_SW_FAST_FLAG = 0x00D9FF11
RAC3_FASTLOAD_SITE = 0x0027B49C
RAC3_FASTLOAD_SITE_WORD = bytes([0x63, 0x43, 0x00, 0x00])
RAC3_LOAD_PLANET = 0x00EE9310
RAC3_MODE = 0x00EE9334


def smoke_rpcs3_rac3_switches(c, pine):
    """
    UYA's Fast loads under RPCS3 now that they are a code switch. A game whose
    RPCS3 patch still carries the Freeze-ammo-only switches is still UYA (the
    fingerprint site's branch did not change) and reads as having switches from
    another build: bit2 set, bit4 clear and a SWITCH_PATCH stamp that is not the
    old one, so the client offers to write the patch again, and meanwhile a
    planet load goes at the game's own pace. With the new patch in, Fast loads
    is a flag byte, and a planet load forces it on and the end of the load (the
    request word clearing) puts it back, the checkbox reading off throughout.
    Starts and ends at XMB.
    """
    old_body = b"".join(struct.pack(">II", a, v) for a, v in RAC3_SW_OLD_ROWS)
    old_body += struct.pack(">HH", len(RAC3_SW_OLD_FLAGS), 0)
    old_body += b"".join(struct.pack(">I", a) + b"\0\0\0\0" for a in RAC3_SW_OLD_FLAGS)
    old_stamp = zlib.crc32(old_body) & 0xFFFFFFFF
    check(old_stamp == 0x0C909623,
          "rpcs3 rac3 switches: the old patch's stamp is the one builds 50 to 53 handed out",
          hex(old_stamp))

    pine.poke(RAC3_FASTLOAD_SITE, RAC3_FASTLOAD_SITE_WORD)
    for addr, value in RAC3_SW_OLD_ROWS:
        pine.poke(addr, struct.pack(">I", value))
    for addr in RAC3_SW_OLD_FLAGS:
        pine.poke(addr, b"\x00")
    pine.poke(RAC3_SW_FAST_FLAG, b"\x00")
    pine.poke(RAC3_LOAD_PLANET, b"\0" * 8)
    pine.boot("NPEA00387", "Ratchet & Clank 3")
    ok, info = wait_state(c, SESSION_INGAME)
    check(ok and info["game"] == 3,
          "rpcs3 rac3 switches: UYA with the Freeze-ammo-only switch patch reaches INGAME as UYA",
          info)

    ok, info = wait_info_flag(c, SESSION_FLAG_CODE_SWITCHES, False)
    check(ok and info["flags"] & SESSION_FLAG_NO_CODE_PATCHES,
          "rpcs3 rac3 switches: bit2 set and bit4 clear, so the client offers the patch",
          hex(info["flags"]) if info else None)

    status, stamp, words, single, body = savefile_patch(c, OP_SWITCH_PATCH)
    check(status == ST_OK and stamp != old_stamp,
          "rpcs3 rac3 switches: SWITCH_PATCH's stamp is not the old patch's: another build",
          (status, hex(stamp) if stamp is not None else None))
    rows = [(a, struct.unpack(">I", v)[0]) for a, v in (words or [])]
    check(len(rows) == 58 and rows[:6] == RAC3_SW_OLD_ROWS[:6],
          "rpcs3 rac3 switches: 58 rows, the ammo trampoline first and unchanged",
          len(rows))
    check(all(0x00980F80 <= a < 0x00981018 for a, _ in rows[6:44]) and
          all(0x00981020 <= a < 0x0098104C for a, _ in rows[44:55]),
          "rpcs3 rac3 switches: the fast-load trampolines' 38 and 11 words at 0x980F80 and 0x981020",
          [hex(a) for a, _ in rows[6:55]])
    check(rows[55:] == [(0x00182A88, 0x487FE478), (0x0027B49C, 0x48705AE4),
                        (0x0027B550, 0x48705AD0)],
          "rpcs3 rac3 switches: and the three sites, the fingerprint's branch as it was",
          [(hex(a), hex(v)) for a, v in rows[55:]])
    check(sorted(a for a, _ in (single or [])) == [0x00D9FF10, RAC3_SW_FAST_FLAG],
          "rpcs3 rac3 switches: two flag bytes, 0xD9FF10 and 0xD9FF11",
          [hex(a) for a, _ in (single or [])])

    # Build 53: the trophy reset can never work here, so DESCRIBE leaves out its
    # button (21) and its toggle (39) rather than offer two rows that always
    # answer UNSUPPORTED.
    status, body = c.call(OP_DESCRIBE)
    ids = set()
    if status == ST_OK:
        ids = {f["id"] for f in parse_describe(body)[3]}
    check(status == ST_OK and 21 not in ids and 39 not in ids and RAC3_SW_FAST_ID in ids,
          "rpcs3 rac3 switches: DESCRIBE leaves out the trophy reset and its toggle",
          (status, sorted(ids)))
    status, _ = c.call(OP_FEATURE_TRIGGER, bytes([21]))
    check(status == ST_NOT_FOUND, "rpcs3 rac3 switches: and the reset's id is NOT_FOUND", status)

    status, _ = c.call(OP_FEATURE_SET, struct.pack(">BI", RAC3_SW_FAST_ID, 1))
    check(status == ST_UNSUPPORTED,
          "rpcs3 rac3 switches: Fast loads is refused until then", status)
    status, _ = c.call(OP_PLANET_LOAD, bytes([5, 0]))
    check(status == ST_OK and pine.peek(RAC3_LOAD_PLANET, 8) == struct.pack(">II", 1, 5),
          "rpcs3 rac3 switches: and a planet load goes ahead at the game's own pace",
          (status, pine.peek(RAC3_LOAD_PLANET, 8).hex()))
    check(pine.peek(RAC3_SW_FAST_FLAG, 1) == b"\x00" and
          pine.peek(RAC3_FASTLOAD_SITE, 4) == RAC3_FASTLOAD_SITE_WORD,
          "rpcs3 rac3 switches: writing no flag and no code word",
          (pine.peek(RAC3_SW_FAST_FLAG, 1).hex(), pine.peek(RAC3_FASTLOAD_SITE, 4).hex()))

    # -------------------------------------- the client rewrites the patch
    pine.shutdown_game()
    ok, _ = wait_state(c, SESSION_XMB)
    check(ok, "rpcs3 rac3 switches: the game is stopped for the new patch")
    pine.poke(RAC3_LOAD_PLANET, b"\0" * 8)
    for addr, value in words:
        pine.poke(addr, value)
    for addr, value in single:
        pine.poke(addr, bytes([value]))
    pine.boot("NPEA00387", "Ratchet & Clank 3")
    ok, info = wait_state(c, SESSION_INGAME)
    check(ok and info["game"] == 3, "rpcs3 rac3 switches: UYA comes back with the new patch", info)
    ok, info = wait_info_flag(c, SESSION_FLAG_CODE_SWITCHES, True)
    check(ok, "rpcs3 rac3 switches: and bit4 CODE_SWITCHES is set",
          hex(info["flags"]) if info else None)

    status, _ = c.call(OP_FEATURE_SET, struct.pack(">BI", RAC3_SW_FAST_ID, 1))
    check(status == ST_OK and pine.peek(RAC3_SW_FAST_FLAG, 1) == b"\x01",
          "rpcs3 rac3 switches: Fast loads goes on through its flag byte", status)
    status, _ = c.call(OP_FEATURE_SET, struct.pack(">BI", RAC3_SW_FAST_ID, 0))
    check(status == ST_OK and pine.peek(RAC3_SW_FAST_FLAG, 1) == b"\x00",
          "rpcs3 rac3 switches: and off", status)

    # A forced load: the flag goes up, the checkbox stays off, and the end of the
    # load puts the flag back.
    del pine.write_log[:]
    status, _ = c.call(OP_PLANET_LOAD, bytes([8, 0]))
    check(status == ST_OK and pine.peek(RAC3_LOAD_PLANET, 8) == struct.pack(">II", 1, 8),
          "rpcs3 rac3 switches: a planet load to Aquatos goes ahead",
          (status, pine.peek(RAC3_LOAD_PLANET, 8).hex()))
    check(pine.peek(RAC3_SW_FAST_FLAG, 1) == b"\x01",
          "rpcs3 rac3 switches: forcing fast loads on through their flag byte",
          pine.peek(RAC3_SW_FAST_FLAG, 1).hex())
    time.sleep(0.4)
    info = fresh_state(c)
    check(info is not None and not info["toggle_state"] & (1 << RAC3_SW_FAST_ID),
          "rpcs3 rac3 switches: while the checkbox reads back off",
          info["toggle_state"] if info else None)
    check(pine.peek(RAC3_SW_FAST_FLAG, 1) == b"\x01",
          "rpcs3 rac3 switches: the flag stays up while the request word is",
          pine.peek(RAC3_SW_FAST_FLAG, 1).hex())
    pine.poke(RAC3_MODE, struct.pack(">I", 6))
    time.sleep(0.4)
    pine.poke(RAC3_LOAD_PLANET, b"\0\0\0\0")
    pine.poke(RAC3_MODE, struct.pack(">I", 0))
    time.sleep(0.4)
    check(pine.peek(RAC3_SW_FAST_FLAG, 1) == b"\x00",
          "rpcs3 rac3 switches: and the load's end, after mode 6, puts it back",
          pine.peek(RAC3_SW_FAST_FLAG, 1).hex())
    info = fresh_state(c)
    check(info is not None and not info["toggle_state"] & (1 << RAC3_SW_FAST_ID),
          "rpcs3 rac3 switches: the checkbox still off",
          info["toggle_state"] if info else None)

    code = set()
    for addr, _ in words:
        code.update(range(addr, addr + 4))
    code.update(range(0x009807F0, 0x009808B0))
    hits = [(hex(a), w) for a, w in pine.write_log if any((a + k) in code for k in range(w))]
    check(not hits, "rpcs3 rac3 switches: no trampoline byte, site word or console cave written",
          hits[:4])

    pine.shutdown_game()
    ok, info = wait_state(c, SESSION_XMB)
    check(ok, "rpcs3 rac3 switches: UYA stops", info)
    for addr, _ in words:
        pine.poke(addr, b"\0\0\0\0")
    pine.poke(RAC3_LOAD_PLANET, b"\0" * 8)


def smoke_rpcs3_held(exe):
    """Another program already in RPCS3's one IPC seat (pine_server.h serves
    one client at a time): the helper must stay responsive to the PC client,
    say what is wrong, and recover on its own once the seat is free."""
    if not os.path.exists(exe):
        return

    root = os.path.join(HERE, "qwark-rpcs3-root")
    shutil.rmtree(root, ignore_errors=True)

    pine = FakePine()
    pine.poke(RAC1_FP_ADDR, RAC1_FP)
    pine.boot("NPEA00385", "Ratchet & Clank")

    # The other client: connected, served, silent.
    holder = socket.create_connection(("127.0.0.1", pine.port), timeout=5)
    time.sleep(0.1)

    # The test's clocks: callers wait 50 ms, a silent link dies after 500 ms,
    # a silent RPCS3 is left alone for 600 ms.
    sim = Sim(exe, ["--pine-port", str(pine.port),
                    "--port", str(RPCS3_PORT),
                    "--root", root,
                    "--pine-timeouts", "50,500,600"])
    sock = None

    try:
        sock = connect(port=RPCS3_PORT)
        c = Client(sock)

        t0 = time.time()
        status, body = c.call(OP_HELLO, bytes([1]))
        took = time.time() - t0
        check(status == ST_OK and took < 1.0,
              "rpcs3 held: HELLO is answered at once while PINE is silent",
              (status, round(took, 3)))
        info = parse_session_info(body) if status == ST_OK else None
        check(info is not None and info["state"] == SESSION_XMB,
              "rpcs3 held: and no game is claimed", info)

        # The tick thread is the one that talks to PINE; if it were parked in
        # a blocking recv these would take seconds each.
        t0 = time.time()
        for _ in range(20):
            c.call(OP_GET_STATE)
        took = time.time() - t0
        check(took < 1.0, "rpcs3 held: twenty requests in under a second",
              round(took, 3))

        time.sleep(1.0)              # past the 500 ms link timeout
        said = [l for l in sim.lines if "has not answered" in l]
        check(len(said) > 0,
              "rpcs3 held: the helper says another program holds the IPC server",
              sim.lines[-3:])
        check(not any("pine: connected" in l for l in sim.lines),
              "rpcs3 held: and never claimed to be connected on the strength of a handshake")

        holder.close()
        ok, info = wait_state(c, SESSION_INGAME, timeout=8.0)
        check(ok, "rpcs3 held: once the other client leaves, the session comes up", info)
        check(any("pine: connected" in l for l in sim.lines),
              "rpcs3 held: and the helper reports the connection then")

        sock.close()
        sock = None

    except Exception as exc:  # noqa: BLE001
        check(False, "rpcs3 held: the smoke run completed without an exception",
              repr(exc))
    finally:
        if sock is not None:
            sock.close()
        try:
            holder.close()
        except OSError:
            pass
        sim.stop()
        pine.stop()



# --------------------------------------------- manip categories, revision 1.21

MANIP_FLAG_NGPLUS = 0x01
MANIP_FLAG_EXTENSION = 0x02
MANIP_FLAG_PER_RUN = 0x04
MANIP_FLAG_NO_CHALLENGE = 0x08

MANIP_EVENT_SETUP = 1
MANIP_EVENT_APPLIED = 2
MANIP_EVENT_NOT_CHALLENGE = 3
MANIP_EVENT_CHALLENGE = 4
MANIP_EVENT_NO_FILE = 5

_NG, _EXT, _RUN, _NOCH = (MANIP_FLAG_NGPLUS, MANIP_FLAG_EXTENSION,
                          MANIP_FLAG_PER_RUN, MANIP_FLAG_NO_CHALLENGE)

# UYA: the planet word, -1 on the title (src/games/rac3.c rac3_in_file).
UYA_IN_FILE = [(0xC1E438, struct.pack(">I", 1))]
UYA_TITLE = [(0xC1E438, struct.pack(">I", 0xFFFFFFFF))]

# One row per game: the groups and headers MANIP_LIST must describe, what a
# save file in play and the title look like in memory, and one group to apply:
# its compatible challenge counter, and a word it writes.
MANIP_SPECS = [
    {
        "title": "NPEA00385", "name": "RaC1",
        "groups": [(1, _NG | _RUN, "NG+ (poki skip)", "NG+"),
                   (2, _NG | _RUN, "NG+ (no poki skip)", "NG+"),
                   (3, _RUN, "All Missions", "All Missions"),
                   (4, _RUN, "All Checkmarks", "All Checkmarks"),
                   (5, _NG | _EXT | _RUN, "NG+ GBSP", "NG+ GBSP")],
        "headers": [],
        "in_file": [(0xA15F38, b"\x00"), (0x969C70, struct.pack(">I", 0))],
        "title_screen": [(0xA15F38, b"\x01")],
        "challenge": (0x96C9FC, 4),
        "apply": (3, 0, 0xA0CD04, 0),
        # Build 53: the file-load trigger, the end of a load into Veldin (src/
        # games/rac1.c): the title, then the request word and destination 0,
        # the transition's state 6 with the front end closing, the request word
        # clearing, and the state leaving 6. All Missions has per-run writes
        # alone, so auto-reset goes back on for it, and its first application
        # in the boot is announced on the console.
        "trigger": {"options": [1, 1],
                    "steps": [[(0x96C9FC, struct.pack(">I", 0)), (0xA15F38, b"\x01"),
                               (0x969C70, struct.pack(">I", 0)),
                               (0xA10708, struct.pack(">I", 3)),
                               (0xA10700, struct.pack(">I", 0))],
                              [(0xA10704, struct.pack(">I", 0)),
                               (0xA10700, struct.pack(">I", 1))],
                              [(0xA10708, struct.pack(">I", 6)), (0xA15F38, b"\x00")],
                              [(0xA10700, struct.pack(">I", 0))],
                              [(0xA10708, struct.pack(">I", 0))]],
                    "check": (0xA0CD07, 0),
                    "toast": "All Missions manips activated"},
    },
    {
        "title": "NPEA00386", "name": "RaC2",
        "groups": [(1, _NG | _RUN, "NG+ no IMG", "NG+ no IMG"),
                   (2, _NG | _RUN, "NG+", "NG+"),
                   (3, _RUN, "Any%", "Any%\nAll Platinum Bolts"),
                   (4, _RUN, "100%", "Max%\n100%"),
                   (5, _NG | _EXT | _RUN, "NG+ All Missions", "NG+ All Missions"),
                   (6, _NG | _EXT | _RUN | _NOCH, "NG+ No Specials", "NG+ No Specials")],
        "headers": [],
        "in_file": [(0x1329A3C, struct.pack(">I", 3))],
        "title_screen": [(0x1329A3C, struct.pack(">I", 0xFFFFFFFF))],
        "challenge": (0x1329AA2, 1),
        "apply": (2, 1, 0x145C180, 30),
    },
    {
        "title": "NPEA00387", "name": "UYA",
        "groups": [(1, _NG, "NG+ no QE", "NG+ no QE"),
                   (2, _NG, "NG+", "NG+"),
                   (3, _NG | _EXT | _RUN, "NG+ CC Early", "NG+ CC Early")],
        "headers": [("Trophies", [21, 39])],
        "in_file": UYA_IN_FILE,
        "title_screen": UYA_TITLE,
        "challenge": (0xC1E50E, 1),
        "apply": (1, 1, 0xC9165C, 7),
        # Build 53: the Trophies header's reset. The auto toggle resets once a
        # load into Veldin is under way, whatever the category - request word,
        # destination, then the load's mode 6 - and the button at once: each
        # deletes the console's folder of UYA's trophy data, found from the
        # game's NP communication id and user 1's home, makes the game's cache
        # fill a nop, and asks the savefile helper's fourth byte for the reset,
        # 2 for "only inside the load" and 1 for "now". "game" is what a booted
        # UYA holds that the reset reads first.
        "trophies": {"toggle": 39, "button": 21, "byte": 0xD9FF1C,
                     "fill": (0x985318, 0x60000000),
                     "folder": ("home", "00000001", "trophy", "NPWR02347_00"),
                     "commid": "NPWR02347_00",
                     "game": [(0xCB150C, struct.pack(">I", 0xD1CC14)),
                              (0xD1CC14 + 0x188, struct.pack(">I", 0)),
                              (0xCBAA04, struct.pack(">I", 0xD05CFC)),
                              (0xD05D00, b"NPWR02347\x00\x00\x00"),
                              (0x985318, struct.pack(">I", 0x9087000C))],
                     "load": [(0xEE9314, struct.pack(">I", 1)),
                              (0xEE9310, struct.pack(">I", 1)),
                              (0xEE9334, struct.pack(">I", 6))],
                     "rest": [(0xEE9310, struct.pack(">I", 0)),
                              (0xEE9314, struct.pack(">I", 0)),
                              (0xEE9334, struct.pack(">I", 0))]},
    },
    {
        "title": "NPEA00423", "name": "Deadlocked",
        "groups": [(1, _NG, "NG+", "NG+ no LC\nNG+")],
        "headers": [("Fixes", [0, 1])],
        "in_file": [(0xB3C7D4, b"\x00"), (0x9C3240, struct.pack(">I", 1))],
        "title_screen": [(0xB3C7D4, b"\x01"), (0x9C3240, struct.pack(">I", 0xFFFFFFFF))],
        "challenge": (0x9C330E, 1),
        # A quit to the XMB is an autosplit PAUSE here, and its RESUME waits for
        # the game's loading hook to say the logo is up: say it, after each boot.
        "after_boot": [(0x1710000, b"\xFF")],
        # The file-load trigger: the tutorial flags' low byte dropping to 0, as a
        # new game or a challenge start does. After the reboot below the setup
        # is due again, so the drop puts it in and announces it.
        "trigger": {"steps": [[(0x9C330E, b"\x01"), (0xB1F46F, b"\x05")],
                              [(0xB1F46F, b"\x00")]],
                    "check": (0xA947D3, 20),
                    "toast": "NG+ manips activated"},
        # One byte of the word: the poison around it stays.
        "apply": (1, 1, 0xA947D0, 0xEEEEEE14),
    },
]


def parse_manip_list(b):
    """MANIP_LIST's reply, or None when it does not parse to its last byte."""
    try:
        sel, auto_reset, remember, ngroups = b[0], b[1], b[2], b[3]
        seq, kind, grp = struct.unpack_from(">HBB", b, 4)
        off = 8
        groups = []
        for _ in range(ngroups):
            gid, flags, n = b[off], b[off + 1], b[off + 2]
            off += 3
            name = b[off:off + n].decode()
            off += n
            n = b[off]
            off += 1
            cats = b[off:off + n].decode()
            off += n
            groups.append((gid, flags, name, cats))
        nheaders = b[off]
        off += 1
        headers = []
        for _ in range(nheaders):
            n = b[off]
            off += 1
            hname = b[off:off + n].decode()
            off += n
            nf = b[off]
            off += 1
            ids = list(struct.unpack_from(">%dH" % nf, b, off))
            off += 2 * nf
            headers.append((hname, ids))
        if off != len(b):
            return None
        return {"selected": sel, "auto_reset": auto_reset, "remember": remember,
                "seq": seq, "kind": kind, "group": grp,
                "groups": groups, "headers": headers}
    except (IndexError, struct.error, UnicodeDecodeError):
        return None


def manip_list(c):
    status, body = c.call(OP_MANIP_LIST)
    return status, (parse_manip_list(body) if status == ST_OK else None)


def exercise_manips(c, sim, spec):
    name = spec["name"]

    sim.send("quit")
    ok, _ = wait_state(c, SESSION_XMB)
    check(ok, "%s manips: the previous game quit" % name)

    status, _ = c.call(OP_MANIP_LIST)
    check(status == ST_UNSUPPORTED, "%s manips: MANIP_LIST is UNSUPPORTED at the XMB" % name,
          status)

    sim.send("boot %s" % spec["title"])
    ok, _ = wait_state(c, SESSION_INGAME)
    if not check(ok, "%s manips: %s reaches INGAME" % (name, spec["title"])):
        return
    for a, v in spec.get("after_boot", []):
        mem_write(c, a, v)

    for addr, value in spec["title_screen"]:
        mem_write(c, addr, value)

    status, l = manip_list(c)
    if not check(status == ST_OK and l is not None,
                 "%s manips: MANIP_LIST answers and parses to its last byte" % name, status):
        return
    check(l["groups"] == spec["groups"],
          "%s manips: the groups are the spec's, ids, flags, names and categories" % name,
          l["groups"])
    check(l["headers"] == spec["headers"], "%s manips: and so are the headers" % name,
          l["headers"])
    check(l["selected"] == 0 and l["auto_reset"] == 1 and l["remember"] == 0,
          "%s manips: None, auto-reset on and remember off out of the box" % name, l)

    status, _ = c.call(OP_MANIP_SELECT, bytes([99]))
    check(status == ST_BAD_ARG, "%s manips: an unknown group is BAD_ARG" % name, status)
    status, _ = c.call(OP_MANIP_SELECT)
    check(status == ST_BAD_ARG, "%s manips: and so is an empty MANIP_SELECT" % name, status)
    status, _ = c.call(OP_MANIP_OPTIONS, bytes([2, 0]))
    check(status == ST_BAD_ARG, "%s manips: an option of 2 is BAD_ARG" % name, status)

    gid, compatible, addr, want = spec["apply"]
    caddr, cwidth = spec["challenge"]
    fmt = ">I" if cwidth == 4 else ">B"

    status, _ = c.call(OP_MANIP_SELECT, bytes([gid]))
    check(status == ST_OK, "%s manips: MANIP_SELECT takes group %d" % (name, gid), status)
    status, l = manip_list(c)
    check(l is not None and l["selected"] == gid, "%s manips: and MANIP_LIST says so" % name)
    seq = l["seq"] if l else 0

    status, _ = c.call(OP_MANIP_APPLY)
    check(status == ST_BUSY, "%s manips: Apply now on the title is BUSY" % name, status)
    status, l = manip_list(c)
    check(l is not None and l["seq"] == (seq + 1) & 0xFFFF and
          l["kind"] == MANIP_EVENT_NO_FILE and l["group"] == gid,
          "%s manips: with event 5, not in a file" % name, l and (l["seq"], l["kind"]))

    for a, v in spec["in_file"]:
        mem_write(c, a, v)
    mem_write(c, caddr, struct.pack(fmt, 1 - compatible))
    mem_write(c, addr, b"\xEE\xEE\xEE\xEE")
    status, _ = c.call(OP_MANIP_APPLY)
    check(status == ST_BUSY, "%s manips: Apply now on the wrong kind of file is BUSY" % name,
          status)
    status, l = manip_list(c)
    refusal = MANIP_EVENT_NOT_CHALLENGE if compatible else MANIP_EVENT_CHALLENGE
    check(l is not None and l["kind"] == refusal,
          "%s manips: with the challenge check's refusal as the event" % name, l and l["kind"])
    check(mem_read_u32(c, addr) == 0xEEEEEEEE, "%s manips: and nothing written" % name)

    mem_write(c, caddr, struct.pack(fmt, compatible))
    status, _ = c.call(OP_MANIP_APPLY)
    check(status == ST_OK, "%s manips: Apply now on a compatible file" % name, status)
    check(mem_read_u32(c, addr) == want,
          "%s manips: writes the group" % name, hex(mem_read_u32(c, addr)))
    status, l = manip_list(c)
    check(l is not None and l["kind"] in (MANIP_EVENT_SETUP, MANIP_EVENT_APPLIED) and
          l["group"] == gid, "%s manips: and records it" % name, l and l["kind"])

    status, _ = c.call(OP_MANIP_OPTIONS, bytes([0, 1]))
    check(status == ST_OK, "%s manips: MANIP_OPTIONS takes auto-reset off, remember on" % name,
          status)
    status, l = manip_list(c)
    check(l is not None and l["auto_reset"] == 0 and l["remember"] == 1,
          "%s manips: and MANIP_LIST reports both" % name, l)

    # A game reboot: remember brings the category back.
    sim.send("quit")
    wait_state(c, SESSION_XMB)
    sim.send("boot %s" % spec["title"])
    ok, _ = wait_state(c, SESSION_INGAME)
    for a, v in spec.get("after_boot", []):
        mem_write(c, a, v)
    status, l = manip_list(c)
    check(ok and l is not None and l["selected"] == gid and l["auto_reset"] == 0,
          "%s manips: a reboot with remember on keeps the category" % name, l)

    trig = spec.get("trigger")
    if trig:
        if trig.get("options"):
            status, _ = c.call(OP_MANIP_OPTIONS, bytes(trig["options"]))
            check(status == ST_OK, "%s manips: MANIP_OPTIONS for the trigger" % name, status)
        seq = l["seq"] if l else 0
        mark = len(sim.lines)
        addr, want = trig["check"]
        mem_write(c, addr, b"\xEE")
        for step in trig["steps"]:
            for a, v in step:
                mem_write(c, a, v)
            time.sleep(0.15)
        deadline = time.time() + 2.0
        while time.time() < deadline and mem_read_u8(c, addr) != want:
            time.sleep(0.05)
        check(mem_read_u8(c, addr) == want,
              "%s manips: the file-load trigger applies the group" % name,
              mem_read_u8(c, addr))
        status, l = manip_list(c)
        check(l is not None and l["seq"] == (seq + 1) & 0xFFFF and
              l["kind"] == MANIP_EVENT_SETUP and l["group"] == gid,
              "%s manips: and records one event, the group activated" % name,
              l and (l["seq"], l["kind"]))
        if trig.get("toast"):
            deadline = time.time() + 2.0
            said = []
            while time.time() < deadline:
                said = [s for s in sim.lines[mark:] if "notify: " in s]
                if said:
                    break
                time.sleep(0.05)
            check(len(said) == 1 and said[0].endswith("notify: " + trig["toast"]),
                  "%s manips: and the console says \"%s\", once" % (name, trig["toast"]),
                  said)

    c.call(OP_MANIP_OPTIONS, bytes([1, 0]))
    c.call(OP_MANIP_SELECT, bytes([0]))

    t = spec.get("trophies")
    if t:
        folder = os.path.join(ROOT, "qwark-host-root", "dev_hdd0", *t["folder"])

        def make_set():
            os.makedirs(folder, exist_ok=True)
            with open(os.path.join(folder, "TROPCONF.SFM"), "w") as f:
                f.write('<trophyconf version="1.1">\n<npcommid>%s</npcommid>\n</trophyconf>\n'
                        % t["commid"])
            for n in ("TROPUSR.DAT", "TROPTRNS.DAT", "TROP000.PNG", "TROP001.PNG"):
                with open(os.path.join(folder, n), "wb") as f:
                    f.write(b"x")

        def wait_byte(want):
            deadline = time.time() + 2.0
            while time.time() < deadline and mem_read_u8(c, t["byte"]) != want:
                time.sleep(0.05)
            return mem_read_u8(c, t["byte"]) == want

        for a, v in t["rest"] + t["game"]:
            mem_write(c, a, v)
        mem_write(c, t["byte"], b"\x00")
        make_set()
        status, _ = c.call(OP_FEATURE_SET, struct.pack(">BI", t["toggle"], 1))
        check(status == ST_OK, "%s trophies: the auto-reset toggle turns on" % name, status)
        for a, v in t["load"]:
            mem_write(c, a, v)
        check(wait_byte(2),
              "%s trophies: a load into Veldin asks the game for the reset, inside the load, "
              "with None selected" % name, mem_read_u8(c, t["byte"]))
        check(not os.path.exists(folder),
              "%s trophies: after deleting the console's trophy folder for the game" % name)
        check(mem_read_u32(c, t["fill"][0]) == t["fill"][1],
              "%s trophies: and making the game's cache fill a nop" % name)
        mem_write(c, t["byte"], b"\x00")          # the helper's answer: done
        for a, v in t["rest"]:
            mem_write(c, a, v)
        c.call(OP_FEATURE_SET, struct.pack(">BI", t["toggle"], 0))

        make_set()
        time.sleep(0.1)
        status, _ = c.call(OP_FEATURE_TRIGGER, bytes([t["button"]]))
        check(status == ST_OK, "%s trophies: the Reset trophies button is accepted" % name, status)
        check(wait_byte(1) and not os.path.exists(folder),
              "%s trophies: and deletes the folder, then asks the game to reset now" % name,
              mem_read_u8(c, t["byte"]))
        mem_write(c, t["byte"], b"\x00")
        time.sleep(0.1)

        # The two rows keep their labels, and no flag asks a client to wait.
        status, body = c.call(OP_DESCRIBE)
        rows = {}
        if status == ST_OK:
            rows = {f["id"]: f for f in parse_describe(body)[3]}
        check(rows.get(t["button"], {}).get("label") == "Reset trophies" and
              rows.get(t["toggle"], {}).get("label") == "Reset trophies on Veldin load",
              "%s trophies: DESCRIBE labels the reset and its toggle" % name,
              {i: rows[i]["label"] for i in (t["button"], t["toggle"]) if i in rows})
        check(not any(f["flags"] & 0x40 for f in rows.values()) and
              rows.get(t["button"], {}).get("flags") == 0,
              "%s trophies: and no row carries flags bit6, the reset no flag at all" % name,
              {i: hex(f["flags"]) for i, f in rows.items() if f["flags"]})
    status, l = manip_list(c)
    check(l is not None and l["selected"] == 0 and l["auto_reset"] == 1 and l["remember"] == 0,
          "%s manips: back to None, auto-reset on, remember off" % name, l)


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

    # No --bind: the simulator's own default is loopback, which is the point.
    sim = Sim(exe, ["--port", str(PORT)])
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind((HOST, 0))
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

        # --port was honoured and, with no --bind given, the default is loopback:
        # nothing the host builds open reaches beyond this machine, which is why
        # no run of this suite raises a Windows Firewall prompt.
        want = "ready on %s:%d" % (HOST, PORT)
        line = sim_line(sim, "ready on ")
        check(line is not None and want in line,
              "the simulator took --port %d and bound %s by default" % (PORT, HOST),
              line)

        # Every request used to leave two lines in the log, one as it arrived and
        # one when it was answered, on unless config.txt said trace_ops = 0.
        # Build 39 took that out: an op is logged nowhere, whatever the config
        # says, and this HDD has no config.txt at all.
        time.sleep(0.5)
        traced = [l for l in sim.lines if "qwark: op " in l]
        check(not traced, "a request leaves no trace line in the log", traced[:2])

        # ------------------------------------------------------- subscribe
        status, _ = c.call(OP_SUBSCRIBE, struct.pack(">H", udp_port))
        check(status == ST_OK, "SUBSCRIBE accepted", status)

        # ------------------------------------------------- what a client holds
        # A connection used to allocate its request and reply buffers when it
        # connected and keep them until it left: 192 KB of the VSH's own memory,
        # held through a game launch, which is when the VSH has to give memory back.
        # Revision 1.11 went further: there is no allocation at all now, one
        # shared 16 KB request buffer and one 16 KB reply buffer live in the
        # module, and a connection holds them for one request. Between requests
        # an idle, connected client must hold nothing.
        #
        # The reply goes out a moment before the arena is let go, so a reading
        # taken the instant a reply lands can still see it: ask until it reads 0.
        held = sim_arena_idle(sim)
        check(held == 0, "a connected client that has finished its requests holds no arena", held)
        for _ in range(5):
            c.call(OP_HELLO, bytes([1]))
        held = sim_arena_idle(sim)
        check(held == 0, "and still none after five more", held)

        # ------------------------------------------- a subscriber that goes quiet
        # A client that sends nothing for five seconds is not sent to, and its next
        # frame starts the packets again. The console used to throw the
        # subscription away instead, and a client that had gone quiet for a moment
        # was left polling over TCP for the rest of its connection.
        time.sleep(5.5)
        drain_udp(udp)
        check(not telemetry_within(udp, 0.5),
              "a subscriber that has sent nothing for five seconds is not sent to")
        status, _ = c.call(OP_HEARTBEAT)
        check(status == ST_OK, "HEARTBEAT answers", status)
        check(telemetry_within(udp, 1.0), "and its next frame brings the telemetry back")

        # ------------------------------------------------------------ boot
        test_boot_buffers(sim, c, udp_port, udp)
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
            check(len(features) == 21, "twenty-one features", len(features))
            check(9 not in by_id, "the retired id 9 is not described", sorted(by_id))
            # Build 53: the five manip buttons are retired the same way.
            check(not any(i in by_id for i in (7, 8, 10, 11, 16)),
                  "nor the five retired manip ids", sorted(by_id))
            check(6 in by_id and 12 in by_id and 18 in by_id,
                  "and the ids around them kept their numbers")
            # Build 53: max ammo, all weapons (17) is retired, Infinite health
            # (2) is Freeze health, held at the game's maximum and writing no
            # code, and Freeze health's own row while it sat beside it, 28, is
            # retired.
            check(17 not in by_id and 28 not in by_id,
                  "nor the retired ids 17 and 28", sorted(by_id))
            fh = by_id.get(RAC1_FREEZE_HEALTH["id"], {})
            check(fh.get("kind") == FEATURE_TOGGLE and fh.get("label") == "Freeze health"
                  and groups[fh.get("group", 0)] == "Cheats"
                  and not fh.get("flags", 0) & FEATURE_FLAG_WRITES_CODE,
                  "Freeze health is 2, a toggle in Cheats that writes no code", fh)
            rows = cheats_rows(groups, features)
            check(rows == FOUR_CHEATS + ["Goodies menu"],
                  "Cheats leads with the four cheats every game has, in order", rows)
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

        exercise_freeze_health(c, "RaC1", RAC1_FREEZE_HEALTH)

        # ------------------------------------------- the savefile block, 1.9
        save_aside = [f for f in by_id.values()
                      if f["flags"] & FEATURE_FLAG_SAVE_ASIDE]
        if check(len(save_aside) == 1, "RaC1 names one SAVE_ASIDE action", save_aside):
            # SF_API_SETASIDE from src/games/sfhelper/sf_rac1.h, which build 53
            # moved out of the game's bump arena.
            exercise_savefile(c, "RaC1", save_aside[0]["id"], RAC1_SF_API_SETASIDE,
                              save=RAC1_SAVE_WRITES)

        # -------------------------------- the console savefile library, 1.10
        # SF_API_SETASIDE and SF_API_LOAD, the two request bytes this stands in
        # for the helper in clearing.
        exercise_savefile_library(c, "RaC1", RAC1_SF_API_SETASIDE, RAC1_SF_API_LOAD,
                                  save=RAC1_SAVE_WRITES, head=RAC1_SAVE_HEAD)

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

        # Build 53: the load forces fast loads on, as RaC2's does (rac1.c).
        for addr, word, _patched in RAC1_FASTLOAD_SITES:
            mem_write(c, addr, struct.pack(">I", word))
        mem_write(c, RAC1_GAME_STATE, struct.pack(">I", 0))
        mem_write(c, RAC1_IN_FRONTEND, b"\x00")

        status, _ = c.call(OP_PLANET_LOAD, bytes([3, 0]))
        check(status == ST_OK, "PLANET_LOAD with no flags", status)
        status, body = c.call(OP_MEM_READ, struct.pack(">II", RAC1_LOAD_PLANET, 8))
        check(body == struct.pack(">II", 1, 3), "the planet request went out", body)
        status, body = c.call(OP_MEM_READ, struct.pack(">II", RAC1_LEVEL_FLAGS + 3 * 0x10, 1))
        check(body == b"\x11", "the level flags were left alone", body)
        check([mem_read_u32(c, a) for a, _w, _p in RAC1_FASTLOAD_SITES]
              == [p for _a, _w, p in RAC1_FASTLOAD_SITES],
              "fast loads were forced on for it",
              [hex(mem_read_u32(c, a)) for a, _w, _p in RAC1_FASTLOAD_SITES])

        # The game's end of the load: the request word back at 0, then the
        # level's state 6 left. Only then do the game's words come back.
        mem_write(c, RAC1_GAME_STATE, struct.pack(">I", 6))
        time.sleep(0.1)
        mem_write(c, RAC1_LOAD_PLANET, struct.pack(">II", 0, 0))
        time.sleep(0.1)
        check(mem_read_u32(c, RAC1_FASTLOAD_SITES[1][0]) == RAC1_FASTLOAD_SITES[1][2],
              "still forced while the level's state 6 runs",
              hex(mem_read_u32(c, RAC1_FASTLOAD_SITES[1][0])))
        mem_write(c, RAC1_GAME_STATE, struct.pack(">I", 0))
        deadline = time.time() + 2.0
        while time.time() < deadline and \
                mem_read_u32(c, RAC1_FASTLOAD_SITES[0][0]) != RAC1_FASTLOAD_SITES[0][1]:
            time.sleep(0.02)
        check([mem_read_u32(c, a) for a, _w, _p in RAC1_FASTLOAD_SITES]
              == [w for _a, w, _p in RAC1_FASTLOAD_SITES],
              "and the end of the load put the game's words back",
              [hex(mem_read_u32(c, a)) for a, _w, _p in RAC1_FASTLOAD_SITES])

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

        # ------------------------------------------- the frame cap, revision 1.11
        test_frame_cap(c)
        test_mem_read_cap(c, 0x00700000)
        test_client_limit(c)
        test_arena_sharing(sim, c, 0x00720000)

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

        # ------------------------------------------- the combo hold, revision 1.8
        # What the client does while it captures a combo: the buttons being
        # recorded reach the console too, so it holds every stored combo off
        # rather than saving a position under the user's fingers.

        def saved_position():
            status, body = c.call(OP_POS_LIST)
            if status != ST_OK:
                return None
            off = 2 + 4 * 16
            if body[off] != 1:
                return None
            return struct.unpack(">3f", body[off + 4:off + 16])

        status, _ = c.call(OP_COMBO_SUSPEND, b"")
        check(status == ST_BAD_ARG, "COMBO_SUSPEND with no payload is BAD_ARG", status)

        status, _ = c.call(OP_COMBO_SUSPEND, bytes([1]))
        check(status == ST_OK, "COMBO_SUSPEND 1 holds the combos off", status)

        coords3 = struct.pack(">fff", 1.0, 2.0, 3.0) + b"\x00" * 18
        c.call(OP_MEM_WRITE, struct.pack(">I", RAC1_COORDS) + coords3)

        sim.send("pad 0x1005")
        time.sleep(0.4)
        sim.send("pad 0x0")
        time.sleep(0.2)

        held = saved_position()
        check(held is not None and abs(held[0] - 9.0) < 0.001,
              "the same combo pressed under the hold saved nothing", held)

        status, _ = c.call(OP_COMBO_SUSPEND, bytes([0]))
        check(status == ST_OK, "COMBO_SUSPEND 0 hands them back", status)

        sim.send("pad 0x1005")
        time.sleep(0.4)
        sim.send("pad 0x0")
        time.sleep(0.2)

        resumed = saved_position()
        check(resumed is not None and abs(resumed[0] - 1.0) < 0.001,
              "and the next press saves a position again", resumed)

        # ---------------------------------------- the combo switch, revision 1.12
        # The user's own on/off, kept on the console instead of in the client:
        # 0 holds every combo off until a later 1, it is in config.txt so it
        # outlives the client and the console, and flags bit3 reports it in
        # every info block, so a checkbox needs no op of its own to read it.
        status, _ = c.call(OP_COMBO_ENABLE, b"")
        check(status == ST_BAD_ARG, "COMBO_ENABLE with no payload is BAD_ARG", status)
        status, _ = c.call(OP_COMBO_ENABLE, bytes([2]))
        check(status == ST_BAD_ARG, "COMBO_ENABLE 2 is BAD_ARG", status)

        state = fresh_state(c)
        check(state and (state["flags"] & SESSION_FLAG_COMBOS_OFF) == 0,
              "the combos start on, so flags bit3 is clear",
              state["flags"] if state else None)

        status, _ = c.call(OP_COMBO_ENABLE, bytes([0]))
        check(status == ST_OK, "COMBO_ENABLE 0 turns every combo off", status)

        state = fresh_state(c)
        check(state and (state["flags"] & SESSION_FLAG_COMBOS_OFF) != 0,
              "GET_STATE reports flags bit3 COMBOS_OFF",
              state["flags"] if state else None)

        drain_udp(udp)
        tflags = None
        deadline = time.time() + 3.0
        while time.time() < deadline:
            try:
                data, _addr = udp.recvfrom(2048)
            except socket.timeout:
                break
            if data[:4] != b"QWRK":
                continue
            tinfo, _tw, _n = parse_telemetry(data)
            tflags = tinfo["flags"]
            break
        check(tflags is not None and (tflags & SESSION_FLAG_COMBOS_OFF) != 0,
              "and a telemetry packet carries the same bit", tflags)

        coords4 = struct.pack(">fff", 4.0, 5.0, 6.0) + b"\x00" * 18
        c.call(OP_MEM_WRITE, struct.pack(">I", RAC1_COORDS) + coords4)

        sim.send("pad 0x1005")
        time.sleep(0.4)
        sim.send("pad 0x0")
        time.sleep(0.2)

        switched_off = saved_position()
        check(switched_off is not None and abs(switched_off[0] - 1.0) < 0.001,
              "the combo pressed with the switch off saved nothing", switched_off)

        status, _ = c.call(OP_COMBO_ENABLE, bytes([1]))
        check(status == ST_OK, "COMBO_ENABLE 1 hands the combos back", status)

        state = fresh_state(c)
        check(state and (state["flags"] & SESSION_FLAG_COMBOS_OFF) == 0,
              "and flags bit3 clears with it",
              state["flags"] if state else None)

        sim.send("pad 0x1005")
        time.sleep(0.4)
        sim.send("pad 0x0")
        time.sleep(0.2)

        switched_on = saved_position()
        check(switched_on is not None and abs(switched_on[0] - 4.0) < 0.001,
              "and a press with the switch on saves a position again", switched_on)

        # Off again, so the reboot below finds it where the user left it.
        status, _ = c.call(OP_COMBO_ENABLE, bytes([0]))
        check(status == ST_OK, "COMBO_ENABLE 0 once more, before the reboot", status)

        # ------------------------------------------------------ mod listing
        status, mods = mod_list(c)
        check(status == ST_OK and len(mods) >= 6,
              "MOD_LIST reports the installed mod fixtures", len(mods))
        check(any(m["name"] == "Hardcore Mode" for m in mods),
              "a mod's #- name came through", [m["name"] for m in mods])

        # Revision 1.16: MOD_PATCH answers on a console too, where it is only
        # informational, and a console never says CHECKING.
        hardcore = next((m for m in mods if m["dirname"] == "hardcore"), None)
        if check(hardcore is not None, "hardcore is in the list for MOD_PATCH"):
            status, stamp, rows, single, body = mod_patch(c, hardcore["index"])
            check(status == ST_OK and rows == [(0x4684B0, b"\x60\x00\x00\x00")] and single == [],
                  "MOD_PATCH hands back hardcore's one word", (status, rows, single))
            check(status == ST_OK and stamp == (zlib.crc32(body[8:]) & 0xFFFFFFFF),
                  "with the CRC-32 stamp over the rest of the reply", stamp)
        status, _ = c.call(OP_MOD_PATCH, b"\x01")
        check(status == ST_BAD_ARG, "MOD_PATCH without its pad bytes is BAD_ARG", status)
        status, _ = c.call(OP_MOD_PATCH, bytes([200, 0, 0, 0]))
        check(status == ST_NOT_FOUND, "and MOD_PATCH of an index past the list NOT_FOUND", status)
        check(all((m["flags"] & MOD_FLAG_CHECKING) == 0 for m in mods),
              "no mod on a console is ever CHECKING", [m["flags"] for m in mods])

        # Revision 1.17: SWITCH_PATCH answers on a console too, where it is only
        # informational, and a console never sets bit4: its toggles patch code.
        status, stamp, rows, single, body = savefile_patch(c, OP_SWITCH_PATCH)
        check(status == ST_OK and len(rows) == 44 and
              [a for a, _ in single] == [0x009058B0, 0x009058B1, 0x009058B2],
              "SWITCH_PATCH hands back RaC1's 44 rows and 3 flag bytes on a console too",
              (status, len(rows) if rows else None))
        check(status == ST_OK and stamp == (zlib.crc32(body[8:]) & 0xFFFFFFFF),
              "stamped with the CRC-32 of the rest of the reply", stamp)
        status, body = c.call(OP_HELLO, bytes([1]))
        info = parse_session_info(body) if status == ST_OK else None
        check(info is not None and (info["flags"] & SESSION_FLAG_CODE_SWITCHES) == 0,
              "and a console never sets SessionInfo bit4", hex(info["flags"]) if info else None)

        # --------------------------------------- an upload while a mod is on
        # What the client's Load button does: write the files, write qwark.sum,
        # MOD_RESCAN, MOD_LOAD. The rescan has to hand back the mod that is on
        # the console now, and it has to do that while other mods are loaded -
        # one the user left on, or an auto one the game brought in by itself.
        # It used to skip every folder it already knew the moment any mod was
        # loaded, so an uploaded mod kept the hash and the words of the copy it
        # replaced: the client compared its own CRC against a hash that could
        # never change and called the mod stale for the rest of the session.
        status, _ = c.call(OP_MOD_LOAD, fixed32("hardcore"))
        check(status == ST_OK, "MOD_LOAD hardcore, a mod the user leaves on", status)

        folder = "/dev_hdd0/qwark/mods/NPEA00385/lock_rng"
        write_console_file(c, folder + "/patch.txt",
                           b"#- name: Lock RNG\r\n"
                           b"#- version: 2.0\r\n"
                           b"#- description: the edited copy\r\n"
                           b"\r\n"
                           b"# a comment line the old copy did not have\r\n"
                           b"0x00A10000: 0x60000000\r\n")
        write_console_file(c, folder + "/qwark.sum", b"0badf00d")
        status, _ = c.call(OP_MOD_RESCAN)
        check(status == ST_OK, "MOD_RESCAN answers OK with a mod loaded", status)

        status, mods = mod_list(c)
        edited = next((m for m in mods if m["dirname"] == "lock_rng"), None)
        check(edited is not None and edited["hash"] == 0x0BADF00D,
              "MOD_LIST reports the hash the upload just wrote",
              "%08x" % edited["hash"] if edited else None)
        check(edited is not None and edited["version"] == "2.0",
              "and the metadata out of the patch.txt that came with it",
              edited["version"] if edited else None)

        status, body = c.call(OP_MOD_INFO, fixed32("lock_rng"))
        check(status == ST_OK and body == b"the edited copy",
              "MOD_INFO reads the new description back", (status, body))

        kept = next((m for m in mods if m["dirname"] == "hardcore"), None)
        check(kept is not None and (kept["flags"] & MOD_FLAG_LOADED) != 0,
              "and the mod that was loaded came through the rescan loaded",
              kept["flags"] if kept else None)

        status, _ = c.call(OP_MOD_UNLOAD, fixed32("hardcore"))
        check(status == ST_OK, "MOD_UNLOAD hands the game back its own words", status)

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
        # Revision 1.12: the switch is console state, not session state, so a
        # boot cycle of the same title finds it exactly as the user left it.
        check(info and (info["flags"] & SESSION_FLAG_COMBOS_OFF) != 0,
              "and the combo switch is still off after the reboot",
              info["flags"] if info else None)
        status, _ = c.call(OP_COMBO_ENABLE, bytes([1]))
        check(status == ST_OK, "COMBO_ENABLE 1 turns the combos back on", status)
        state = fresh_state(c)
        check(state and (state["flags"] & SESSION_FLAG_COMBOS_OFF) == 0,
              "leaving flags bit3 clear for the rest of the run",
              state["flags"] if state else None)
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

        # ------------------------------------------------- autosplitting
        # RaC1 splits when the destination planet changes to another real one.
        exercise_autosplit(c, udp, "RaC1", {
            "rows": [
                (1, AUTOSPLIT_SPLIT, DF | RT, 0, "Planet entered"),
                (2, AUTOSPLIT_SPLIT, DF, 0, "Veldin"),
                (3, AUTOSPLIT_SPLIT, DF, 0, "Drek button"),
                (8, AUTOSPLIT_LOAD_START, DF | NM, 7560000, "Loading screen"),
            ],
            "setup": [(0x969C70, struct.pack(">I", 3)),
                      (0xA10704, struct.pack(">I", 0))],
            "trigger": [(0xA10704, struct.pack(">I", 5))],
            "code": 1,
            "arg": 5,
            # The loading screen: anything but 4 is a load, and the pair bounds
            # the interval the client normalises to 7.56 s.
            "timing": [
                {"poke": [(0x9645CB, b"\x04")],
                 "then": [(0x9645CB, b"\x00")],
                 "expect": (AUTOSPLIT_LOAD_START, 8),
                 "arg": 0,
                 "what": "leaving loading-screen id 4 emits LOAD_START"},
                {"then": [(0x9645CB, b"\x02")],
                 "absent": [(AUTOSPLIT_LOAD_START, 8), (AUTOSPLIT_LOAD_END, 8)],
                 "what": "a change between two loading ids emits nothing"},
                {"then": [(0x9645CB, b"\x04")],
                 "expect": (AUTOSPLIT_LOAD_END, 8),
                 "arg": 4,
                 "what": "and coming back to 4 emits LOAD_END"},
            ],
        })

        # RaC1's embedded collectable helper is disabled while boot crashes
        # are investigated. Read-only events can still consume externally set counters.
        for address in (0x708EC8, 0x11B7C0, 0x112F08, 0x112CD0,
                        0x4F5BE4, 0x4F5CAC, 0x4F5D10, 0x4F5D74):
            check(mem_read_u32(c, address) == 0,
                  "RaC1: autosplit hook/cave remains untouched at %x" % address)

        # An unsupported option must not split after disappearing from the menu.
        mark, _events, _n = autosplit_events(c, 0)
        mem_write(c, 0xAFF000, struct.pack(">I", 0x21))
        check(as_absent(c, mark, AUTOSPLIT_SPLIT, 4),
              "RaC1: retired gold-bolt option emits no hidden split")

        # -------------------------------------- RaC2, RaC3 and Deadlocked
        for spec in OTHER_GAMES:
            exercise_game(c, sim, spec, udp)

        # ------------------------------- manip categories, revision 1.21
        for spec in MANIP_SPECS:
            exercise_manips(c, sim, spec)

        # --------------------- Deadlocked's quit to the XMB, revision 1.5
        # PAUSE on the way out, and RESUME only once the game's own loading
        # hook says the SCE logo is up: the process comes back seconds before
        # the game is playable, and the old script never gave those away.
        mark, _events, _n = autosplit_events(c, 0)
        sim.send("quit")
        ok, _ = wait_state(c, SESSION_XMB)
        check(ok, "Deadlocked: it quits to the XMB")
        paused = as_wait(c, mark, AUTOSPLIT_PAUSE, 3)
        check(paused is not None, "Deadlocked: and that emits PAUSE with code 3")
        check(as_absent(c, mark, AUTOSPLIT_RESUME, 3),
              "Deadlocked: with no RESUME yet")

        mark, _events, _n = autosplit_events(c, 0)
        sim.send("boot NPEA00423")
        ok, _ = wait_state(c, SESSION_INGAME)
        check(ok, "Deadlocked: it comes back")
        check(as_absent(c, mark, AUTOSPLIT_RESUME, 3),
              "Deadlocked: the process coming back is not yet a RESUME")
        check(mem_read_u32(c, 0x11884) == 0x48000080,
              "Deadlocked: the loading hook is patched in on entry")
        check(mem_read_u32(c, 0x11904) == 0x9421FFF0,
              "Deadlocked: and its trampoline with it")

        mem_write(c, 0x1710000, b"\xFF")
        resumed = as_wait(c, mark, AUTOSPLIT_RESUME, 3)
        if check(resumed is not None,
                 "Deadlocked: the loading hook's 0xFF emits RESUME with code 3"):
            check(mem_read_u32(c, 0x1710000) >> 24 == 0,
                  "Deadlocked: and the byte is cleared for the next quit")

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
            check(len(features) == 28, "with RaC3's twenty-eight features",
                  len(features))

        # ------------------------ BLMODYOOO, a repacked PAL Going Commando
        # The old client accepted two repacks of NPEA00386 by their own title
        # ids. They are the same build, so they answer RaC2's fingerprint and
        # get RaC2's whole table.
        sim.send("quit")
        ok, _ = wait_state(c, SESSION_XMB)
        check(ok, "quit before the RaC2 repack")

        sim.send("boot BLMODYOOO")
        ok, info = wait_state(c, SESSION_INGAME)
        check(ok, "boot BLMODYOOO reaches INGAME", info)
        check(info and info["title"] == "BLMODYOOO",
              "the repack keeps its own title id", info)
        check(info and info["game"] == 2, "and the game byte says RaC2",
              info["game"] if info else None)

        status, body = c.call(OP_DESCRIBE)
        if check(status == ST_OK, "DESCRIBE answers under BLMODYOOO", status):
            game, _groups, _readouts, features, _consumed = parse_describe(body)
            check(game == 2, "and names RaC2", game)
            check(len(features) == 28, "with RaC2's twenty-eight features",
                  len(features))

        # ------------------------------- a title no game table claims
        # Build 36, PROTOCOL.md section 3.3: it reaches INGAME with game 0, the
        # memory tools work on it and everything else answers UNSUPPORTED.
        sim.send("quit")
        ok, _ = wait_state(c, SESSION_XMB)
        check(ok, "quit before the unknown title")

        sim.send("boot BLUS99999")
        ok, info = wait_state(c, SESSION_INGAME)
        check(ok, "a title no game table claims still reaches INGAME", info)
        check(info and info["game"] == 0, "with game 0, no game",
              info["game"] if info else None)
        check(info and info["title"] == "BLUS99999",
              "and the title id it was launched with", info)

        status, body = c.call(OP_HELLO, bytes([1]))
        hello = parse_session_info(body) if status == ST_OK else None
        check(hello and hello["state"] == SESSION_INGAME and hello["game"] == 0
              and hello["title"] == "BLUS99999",
              "HELLO says the same: INGAME, no game, that title", hello)
        check(hello and hello["planet"] == 0 and hello["pad_mask"] == 0
              and hello["pos"] == (0.0, 0.0, 0.0) and not any(hello["readout"]),
              "and no planet, position, pad or readout is filled in", hello)

        # The memory panel is the whole point: write, read it back, watch it.
        check(mem_write(c, 0x00820000, struct.pack(">I", 0xC0FFEE00)) == ST_OK,
              "MEM_WRITE answers OK against a process qwark does not know")
        check(mem_read_u32(c, 0x00820000) == 0xC0FFEE00,
              "and MEM_READ brings the bytes back")

        status, body = c.call(OP_WATCH_ADD, struct.pack(">IB", 0x00820004, 4))
        watch_id = body[0] if status == ST_OK and body else None
        check(watch_id is not None, "WATCH_ADD answers with an id", status)
        mem_write(c, 0x00820004, struct.pack(">I", 0x0000BEEF))

        drain_udp(udp)
        got = None
        deadline = time.time() + 3.0
        while time.time() < deadline and watch_id is not None:
            try:
                data, _addr = udp.recvfrom(2048)
            except socket.timeout:
                break
            if data[:4] != b"QWRK":
                continue
            tinfo, twatches, _n = parse_telemetry(data)
            row = next((w for w in twatches if w["id"] == watch_id), None)
            if row and row["valid"] and row["value"] == 0x0000BEEF:
                got = (tinfo, row)
                break
        check(got is not None,
              "a telemetry packet carries the watch, valid and live")
        if got:
            check(got[0]["state"] == SESSION_INGAME and got[0]["game"] == 0,
                  "and still says INGAME with no game", got[0]["state"])

        status, _ = c.call(OP_MOBY_TABLE)
        check(status == ST_UNSUPPORTED, "MOBY_TABLE is UNSUPPORTED", status)
        status, _ = c.call(OP_PLANET_LIST)
        check(status == ST_UNSUPPORTED, "PLANET_LIST is UNSUPPORTED", status)
        status, _ = c.call(OP_UNLOCK_LIST)
        check(status == ST_UNSUPPORTED, "UNLOCK_LIST is UNSUPPORTED", status)
        status, _ = c.call(OP_FEATURE_SET, struct.pack(">BI", 0, 1))
        check(status == ST_UNSUPPORTED, "FEATURE_SET is UNSUPPORTED", status)
        status, body = c.call(OP_DESCRIBE)
        check(status == ST_UNSUPPORTED and len(body) == 0,
              "DESCRIBE is UNSUPPORTED, exactly as it is at the XMB",
              (status, len(body)))
        status, _ = c.call(OP_MANIP_LIST)
        check(status == ST_UNSUPPORTED, "MANIP_LIST is UNSUPPORTED: no manip groups", status)
        status, _ = c.call(OP_MANIP_APPLY)
        check(status == ST_UNSUPPORTED, "and so is MANIP_APPLY", status)

        sim.send("quit")
        ok, info = wait_state(c, SESSION_XMB)
        check(ok, "and the process going away returns it to the XMB")
        check(info and info["game"] == 0 and info["title"] == "",
              "clearing the game and the title", info)

        status, body = c.call(OP_DESCRIBE)
        check(status == ST_UNSUPPORTED,
              "DESCRIBE is UNSUPPORTED with no game running", status)

        status, body = c.call(OP_AUTOSPLIT_DESCRIBE)
        check(status == ST_UNSUPPORTED and len(body) == 0,
              "AUTOSPLIT_DESCRIBE is UNSUPPORTED with no game running",
              (status, len(body)))

        # The ring outlives the game: a client reconnecting still gets the run.
        latest, events, consumed = autosplit_events(c, 0)
        check(latest is not None and latest > 0,
              "AUTOSPLIT_EVENTS still answers with no game running", latest)
        check(consumed >= 0, "and its reply parses exactly", consumed)
        check(len(events) > 0 and events[-1]["seq"] == latest,
              "with the ring's newest event last", len(events))
        check(len(events) <= 64, "and at most 64 entries", len(events))

        status, _ = c.call(OP_AUTOSPLIT_EVENTS, b"\x00\x00")
        check(status == ST_BAD_ARG,
              "a short AUTOSPLIT_EVENTS payload is BAD_ARG", status)

        sock.close()

    except Exception as exc:  # noqa: BLE001
        check(False, "the smoke run completed without an exception", repr(exc))
    finally:
        udp.close()
        sim.stop()

    # The RPCS3 half: the same core over PINE, against a fake RPCS3.
    print()
    smoke_rpcs3(os.path.join(os.path.dirname(exe) or ROOT, "qwark-rpcs3.exe"))
    smoke_rpcs3_held(os.path.join(os.path.dirname(exe) or ROOT, "qwark-rpcs3.exe"))

    print()
    print("%d passed, %d failed" % (passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
