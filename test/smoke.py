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

PORT = 9673
HOST = "127.0.0.1"

# ---------------------------------------------------------------- protocol

# QWARK_BUILD in src/core/proto.h: the module build number, bumped whenever the
# feature tables or any user-visible behaviour change.
QWARK_BUILD = 34

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
OP_COMBO_SUSPEND = 0x0082
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
OP_FILE_RENAME = 0x0079
OP_SAVEFILE_CATEGORIES = 0x00B3
OP_SAVEFILE_LIST = 0x00B4
OP_SAVEFILE_STORE = 0x00B5
OP_SAVEFILE_RESTORE = 0x00B6
OP_SAVEFILE_CATEGORY = 0x00B7
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
SAVEFILE_NAME_LEN = 32
SAVEFILE_ROW_SIZE = 40
SAVEFILE_CATEGORY_CREATE = 0
SAVEFILE_CATEGORY_DELETE = 1

SESSION_XMB, SESSION_BOOTING, SESSION_INGAME, SESSION_QUITTING = 0, 1, 2, 3

# SessionInfo.flags. Revision 1.6 added the two platform bits: EMULATOR says
# qwark is driving RPCS3 rather than a console, NO_CODE_PATCHES says every
# WRITES_CODE feature is refused here and a client should grey those rows.
SESSION_FLAG_PREVIOUS_PENDING = 0x01
SESSION_FLAG_EMULATOR = 0x02
SESSION_FLAG_NO_CODE_PATCHES = 0x04

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


def sim_pages(sim, timeout=3.0):
    """How many page allocations the simulator has live, via its `pages` command."""
    mark = len(sim.lines)
    sim.send("pages")
    deadline = time.time() + timeout
    while time.time() < deadline:
        for line in sim.lines[mark:]:
            if line.startswith("ok pages "):
                return int(line.split()[2])
        time.sleep(0.02)
    return None


def sim_pages_idle(sim, timeout=1.0):
    """The live page count once the last request has let go of its buffers, or what it
    still was when `timeout` ran out."""
    deadline = time.time() + timeout
    pages = sim_pages(sim)
    while pages != 0 and time.time() < deadline:
        time.sleep(0.01)
        pages = sim_pages(sim)
    return pages


def sim_allocations(sim):
    mark = len(sim.lines)
    sim.send("page_allocs")
    deadline = time.time() + 3
    while time.time() < deadline:
        for line in sim.lines[mark:]:
            if line.startswith("ok page_allocs "):
                return int(line.split()[2])
        time.sleep(.01)
    raise RuntimeError("no allocation count from simulator")


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
    before = sim_allocations(sim)
    for _ in range(5):
        c.call(OP_HELLO, b"\x01")
        c.call(OP_HEARTBEAT)
        c.call(OP_GET_STATE)
    check(sim_allocations(sim) == before, "control traffic never allocates request pages")

    # Every fixed-buffer opcode, including a SUBSCRIBE whose payload straddles
    # the pause. It is already 1.15 seconds old when the second-long pause starts.
    controls = [(OP_HELLO, b"\x01"), (OP_HEARTBEAT, b""),
                (OP_SUBSCRIBE, struct.pack(">H", udp_port)),
                (OP_UNSUBSCRIBE, b""), (OP_GET_STATE, b"")]
    peers = [connect() for _ in controls]
    for peer in peers:
        Client(peer).call(OP_HEARTBEAT)
    peers[2].sendall(struct.pack(">IHH", 2, 71, OP_SUBSCRIBE) + controls[2][1][:1])
    time.sleep(1.15)
    partial = connect()
    try:
        # MOD_LOAD, with the entire payload delayed. HELLO now uses fixed buffers.
        partial.sendall(struct.pack(">IHH", 32, 1, 0x61))
        deadline = time.time() + 1
        while sim_pages(sim) == 0 and time.time() < deadline:
            time.sleep(.01)
        check(sim_pages(sim) == 1, "partial bulk request acquires one allocation outside boot")
        before = sim_allocations(sim)
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
        for i, (op, payload) in enumerate(controls):
            if i == 2:
                peers[i].sendall(payload[1:])
            else:
                peers[i].sendall(struct.pack(">IHH", len(payload), 71, op) + payload)
        ready, _, _ = select.select(peers + [udp], [], [], .65)
        check(not ready, "all control requests and UDP remain silent inside the first second")
        check(sim_activity(sim) == activity, "no PID, title, memory or telemetry activity during the pause")
        check(sim_pages_idle(sim, .1) == 0, "cancelled bulk buffers are released during the quiet window")
        check(sim_allocations(sim) == before, "quiet-window traffic allocates no request pages")
        for peer in peers:
            response = Client(peer)
            length, seq, status = struct.unpack(">IHH", response.recv_exact(8))
            if length:
                response.recv_exact(length)
            check(seq == 71 and status == ST_OK, "deferred control frame resumes with its original sequence")
        check(wait_state(c, SESSION_BOOTING)[0], "allocation gate test reaches BOOTING")
        check(sim_pages_idle(sim, 1.0) == 0, "launch cancels and releases an incomplete bulk request")
        try:
            disconnected = partial.recv(1) == b""
        except (ConnectionResetError, ConnectionAbortedError):
            disconnected = True
        check(disconnected, "cancelled bulk connection closes")

        for op, payload in [(OP_DESCRIBE, b""), (OP_FILE_WRITE, bytes(65600)),
                            (OP_SAVEFILE_INFO, b"")]:
            status, body = c.call(op, payload)
            check(status == ST_BUSY and body == b"", "boot refuses allocating op %d with BUSY" % op, status)
        check(c.call(OP_HELLO, b"\x01")[0] == ST_OK, "HELLO still works in boot")
        check(c.call(OP_HEARTBEAT)[0] == ST_OK, "HEARTBEAT still works after draining a large frame")
        check(c.call(OP_SUBSCRIBE, struct.pack(">H", udp_port))[0] == ST_OK,
              "SUBSCRIBE still works in boot")
        check(c.call(OP_GET_STATE)[0] == ST_OK, "GET_STATE still works in boot")
        check(sim_allocations(sim) == before, "boot traffic made zero page allocations")

        newcomer = connect()
        try:
            try:
                newcomer.sendall(struct.pack(">IHHB", 1, 1, OP_HELLO, 1))
                closed = newcomer.recv(1) == b""
            except (ConnectionResetError, ConnectionAbortedError):
                closed = True
            check(closed, "boot refuses new connection-thread allocations")
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
        check(sim_pages(sim) == 1, "timeout probe starts with a live allocation")
        check(sim_pages_idle(sim, 3.0) == 0, "payload timeout releases its allocation without a boot")
        check(c.call(OP_HEARTBEAT)[0] == ST_OK, "other connections survive a stalled request")
    finally:
        stalled.close()


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
        "features": 37,
        # SF_API_SETASIDE from src/games/sfhelper/sf_rac2.h.
        "sf_setaside": 0x010CD71F,
        # Retired, never renumbered: 32 and 33, the two tempsave manager rows.
        "retired": [32, 33],
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
            ],
        },
    },
    {
        "title": "NPEA00387",
        "name": "RaC3",
        "game": 3,
        "features": 32,
        # SF_API_SETASIDE from src/games/sfhelper/sf_rac3.h.
        "sf_setaside": 0x00D9FF02,
        # Retired, never renumbered: 4, 17, 28, 29, 30 and 33.
        "retired": [4, 17, 28, 29, 30, 33],
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
        "load_expect": [(0xEE9310, 1), (0xEE9314, 5), (0x134EBD4, 3)],
        # Build 11: the Fast loads toggle, and the two words it keeps written.
        # `from` and `to` are Veldin and Tyhrranosis, neither of them Aquatos.
        "fastload": {
            "toggle": 38,
            "value1": 0x134EBD4,
            "value2": 0x134EE70,
            "planet_addr": 0xC1E438,
            "dest_addr": 0xEE9314,
            "from": 5,
            "to": 9,
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
            ],
        },
    },
    {
        "title": "NPEA00423",
        "name": "Deadlocked",
        "game": 4,
        "features": 17,
        # SF_API_SETASIDE from src/games/sfhelper/sf_rac4.h.
        "sf_setaside": 0x015CD71F,
        # Retired, never renumbered: 16, "Reset all weapon levels".
        "retired": [16],
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
        },
    },
]


def exercise_savefile(c, name, save_aside_id, setaside_addr=None):
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
    """
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

    status, _ = c.call(OP_SAVEFILE_READ, struct.pack(">II", 0, 65537))
    check(status == ST_BAD_ARG,
          "%s: a read longer than one chunk is BAD_ARG" % name, status)

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
        status, chunk = c.call(OP_FILE_READ, handle + struct.pack(">I", 65536))
        if status != ST_OK:
            break
        data += chunk
        if len(chunk) < 65536:
            break

    c.call(OP_FILE_CLOSE, handle)
    return data


def write_console_file(c, path, data):
    status, body = c.call(OP_FILE_OPEN, b"\x01" + path.encode())
    if status != ST_OK:
        return False

    handle = body[:4]
    for off in range(0, len(data), 65536):
        status, _ = c.call(OP_FILE_WRITE, handle + data[off:off + 65536])
        if status != ST_OK:
            c.call(OP_FILE_CLOSE, handle)
            return False

    c.call(OP_FILE_CLOSE, handle)
    return True


def exercise_savefile_library(c, name, setaside_addr, load_addr):
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
    """
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
    payload = bytes(((i * 31 + 7) & 0xFF) for i in range(65536))
    written = b""
    for off in range(0, size, 65536):
        chunk = payload[:min(65536, size - off)]
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

        # Protocol 1.9: that pair, and the block that moves the bytes.
        if save_aside:
            exercise_savefile(c, name, save_aside[0]["id"], spec.get("sf_setaside"))

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

    # -------------------------------------------------------- planet load
    for addr, _want in spec["load_expect"]:
        mem_write(c, addr, struct.pack(">I", 0))
    status, _ = c.call(OP_PLANET_LOAD, bytes([spec["planet"], 0]))
    if check(status == ST_OK, "%s: PLANET_LOAD accepted" % name, status):
        for addr, want in spec["load_expect"]:
            check(mem_read_u32(c, addr) == want,
                  "%s: the load request word at 0x%X is %d" % (name, addr, want),
                  mem_read_u32(c, addr))

    # --------------------------------------------------- fast loads (1.9, build 11)
    #
    # UYA's two fast-load values are game data rather than patched code, and the
    # game writes its own over them every time it loads a planet. The toggle
    # therefore has to arm them again around every load, including one the game
    # starts on its own: qwark sees that only as the destination planet moving in
    # the hot block, which is what this pokes.
    if spec.get("fastload"):
        fl = spec["fastload"]

        mem_write(c, fl["planet_addr"], struct.pack(">I", fl["from"]))
        mem_write(c, fl["dest_addr"], struct.pack(">I", fl["from"]))
        time.sleep(0.15)
        mem_write(c, fl["value1"], struct.pack(">I", 0))

        status, _ = c.call(OP_FEATURE_SET, struct.pack(">BI", fl["toggle"], 1))
        if check(status == ST_OK, "%s: the Fast loads toggle turns on" % name, status):
            check(mem_read_u32(c, fl["value1"]) == 3,
                  "%s: and arms the fast-load values there and then" % name,
                  mem_read_u32(c, fl["value1"]))

        # The game goes somewhere nobody asked qwark for, clearing them on the way.
        mem_write(c, fl["value1"], struct.pack(">I", 0))
        mem_write(c, fl["value2"], b"\x00\x00")
        mem_write(c, fl["dest_addr"], struct.pack(">I", fl["to"]))
        time.sleep(0.4)

        check(mem_read_u32(c, fl["value1"]) == 3,
              "%s: a planet load the game started re-arms them" % name,
              mem_read_u32(c, fl["value1"]))
        status, body = c.call(OP_MEM_READ, struct.pack(">II", fl["value2"], 2))
        check(status == ST_OK and body == b"\x01\x01",
              "%s: second value and all, a fifth of a second later" % name, body)

        # Off again, so the autosplit steps below poke the same words in peace.
        c.call(OP_FEATURE_SET, struct.pack(">BI", fl["toggle"], 0))
        mem_write(c, fl["planet_addr"], struct.pack(">I", fl["to"]))
        time.sleep(0.15)

    # ------------------------------------------------------ autosplitting
    if udp is not None and spec.get("autosplit"):
        exercise_autosplit(c, udp, name, spec["autosplit"])


# ------------------------------------------------------------------- RPCS3

# The RaC1 fingerprint from src/games/rac1.h and rac1.c, in PS3 byte order: the
# session will not leave BOOTING until it reads these four bytes back.
RAC1_FP_ADDR = 0x0007F558
RAC1_FP = bytes([0x30, 0x64, 0x9C, 0xE0])

# A port of its own, so this can run beside the host smoke above.
RPCS3_PORT = 9674


def smoke_rpcs3(exe):
    """qwark-rpcs3.exe against the fake PINE server: flags, boot, DESCRIBE."""
    if not os.path.exists(exe):
        check(False, "%s exists, run build-host.sh first" % os.path.basename(exe))
        return

    root = os.path.join(HERE, "qwark-rpcs3-root")
    shutil.rmtree(root, ignore_errors=True)

    pine = FakePine()
    pine.poke(RAC1_FP_ADDR, RAC1_FP)

    sim = Sim(exe, ["--pine-port", str(pine.port),
                    "--port", str(RPCS3_PORT),
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

        # The savefile helper is a code cave and a branch into it, so there is
        # nothing here to install and nothing to talk to (protocol 1.9).
        status, _ = c.call(OP_SAVEFILE_INFO)
        check(status == ST_UNSUPPORTED, "rpcs3: SAVEFILE_INFO is UNSUPPORTED", status)
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

        # trace_ops is on unless config.txt says otherwise, and this HDD has no
        # config.txt: the HELLO above is in the log, arriving and answered.
        deadline = time.time() + 2.0
        while time.time() < deadline and not any("qwark: op 1 done" in l for l in sim.lines):
            time.sleep(0.02)
        check(any("qwark: op 1 seq" in l for l in sim.lines) and
              any("qwark: op 1 done, status 0" in l for l in sim.lines),
              "with no config the log traces every request, arriving and answered")

        # ------------------------------------------------------- subscribe
        status, _ = c.call(OP_SUBSCRIBE, struct.pack(">H", udp_port))
        check(status == ST_OK, "SUBSCRIBE accepted", status)

        # ------------------------------------------------- what a client holds
        # A connection used to allocate its request and reply buffers when it
        # connected and keep them until it left: 192 KB of the VSH's own memory,
        # held through a game launch, which is when the VSH has to give memory back.
        # They exist for one request now. Between requests an idle, connected
        # client must hold no pages at all.
        #
        # The reply goes out a moment before its buffers are freed, so a count taken
        # the instant a reply lands can still see them: ask until it reads 0.
        pages = sim_pages_idle(sim)
        check(pages == 0, "a connected client that has finished its requests holds no pages", pages)
        for _ in range(5):
            c.call(OP_HELLO, bytes([1]))
        pages = sim_pages_idle(sim)
        check(pages == 0, "and still none after five more", pages)

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

        # ------------------------------------------- the savefile block, 1.9
        save_aside = [f for f in by_id.values()
                      if f["flags"] & FEATURE_FLAG_SAVE_ASIDE]
        if check(len(save_aside) == 1, "RaC1 names one SAVE_ASIDE action", save_aside):
            # SF_API_SETASIDE from src/games/sfhelper/sf_rac1.h.
            exercise_savefile(c, "RaC1", save_aside[0]["id"], 0x00B00072)

        # -------------------------------- the console savefile library, 1.10
        # SF_API_SETASIDE and SF_API_LOAD, the two request bytes this stands in
        # for the helper in clearing.
        exercise_savefile_library(c, "RaC1", 0x00B00072, 0x00B00071)

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
