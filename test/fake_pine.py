#!/usr/bin/env python3
"""
A fake RPCS3 PINE server, reusable from any test.

It speaks exactly the packet format RPCS3's 3rdparty/pine/pine_server.h speaks:

    request:  u32 total_size (little-endian, counts these four bytes)
              { u8 opcode, args... } one or more, back to back
    reply:    u32 total_size, u8 result (0x00 OK, 0xFF FAIL), each command's
              result concatenated in request order

and reproduces the two details that matter to a client:

  * a read returns the LOGICAL value of the word, little-endian, because RPCS3
    converts the big-endian `be_t` out of guest memory before copying it onto
    the wire. The memory here is therefore held in PS3 byte order and swapped
    on the way out, exactly as the emulator does.
  * MsgStatus advances the request cursor by four even though it takes no
    argument (`buf_cnt += 4` in pine_server.h), so anything batched behind it
    needs four bytes of padding.

One failing command fails the whole packet with a bare five-byte FAIL reply.

Standalone:  python test/fake_pine.py [--port 28012] [--id NPEA00385]
"""

import argparse
import socket
import struct
import threading

MSG_READ8, MSG_READ16, MSG_READ32, MSG_READ64 = 0x00, 0x01, 0x02, 0x03
MSG_WRITE8, MSG_WRITE16, MSG_WRITE32, MSG_WRITE64 = 0x04, 0x05, 0x06, 0x07
MSG_VERSION = 0x08
MSG_TITLE, MSG_ID, MSG_UUID, MSG_GAMEVERSION, MSG_STATUS = 0x0B, 0x0C, 0x0D, 0x0E, 0x0F

IPC_OK, IPC_FAIL = 0x00, 0xFF

STATUS_RUNNING, STATUS_PAUSED, STATUS_SHUTDOWN = 0, 1, 2

DEFAULT_PORT = 28012


class FakePine:
    """A PINE server on 127.0.0.1, backed by a sparse byte array in PS3 order.

    Addresses outside `mapped` answer FAIL, the way RPCS3's check_addr does for
    an unmapped page. By default everything is mapped and unwritten bytes read
    as zero, which is what a smoke test wants.
    """

    def __init__(self, port=0, title_id="", title="", mapped=None):
        self.mem = {}                 # address -> byte, PS3 byte order
        self.mapped = mapped          # None = the whole address space
        self.status = STATUS_SHUTDOWN
        self.title_id = title_id
        self.title = title
        self.version = "RPCS3 fake-pine"

        self.packets = 0
        self.connections = 0
        self.drop = False             # set to hang up on the live connection

        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(("127.0.0.1", port))
        self._sock.listen(4)
        self._sock.settimeout(0.05)
        self.port = self._sock.getsockname()[1]

        self._stop = False
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    # ---------------------------------------------------------------- memory

    def poke(self, addr, data):
        """Writes raw bytes, i.e. PS3 byte order, at `addr`."""
        for i, b in enumerate(data):
            self.mem[addr + i] = b

    def peek(self, addr, length):
        return bytes(self.mem.get(addr + i, 0) for i in range(length))

    def boot(self, title_id, title=""):
        """Pretends a game came up: RPCS3 answers Running and this title id."""
        self.title_id = title_id
        self.title = title or title_id
        self.status = STATUS_RUNNING

    def shutdown_game(self):
        self.title_id = ""
        self.title = ""
        self.status = STATUS_SHUTDOWN

    def _valid(self, addr, size):
        if self.mapped is None:
            return addr + size <= 0x100000000
        lo, hi = self.mapped
        return lo <= addr and addr + size <= hi

    # ------------------------------------------------------------- the server

    def _run(self):
        while not self._stop:
            try:
                conn, _ = self._sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            self.connections += 1
            conn.settimeout(0.05)
            try:
                self._serve(conn)
            finally:
                self.drop = False
                try:
                    conn.close()
                except OSError:
                    pass

    def _recv_exact(self, conn, n):
        buf = b""
        while len(buf) < n:
            if self._stop or self.drop:
                return None
            try:
                chunk = conn.recv(n - len(buf))
            except socket.timeout:
                continue
            except OSError:
                return None
            if not chunk:
                return None
            buf += chunk
        return buf

    def _serve(self, conn):
        while not self._stop and not self.drop:
            head = self._recv_exact(conn, 4)
            if head is None:
                return
            total = struct.unpack("<I", head)[0]
            if total < 4 or total > 650000:
                return
            body = self._recv_exact(conn, total - 4) if total > 4 else b""
            if body is None:
                return

            reply = self.parse(body)
            self.packets += 1
            try:
                conn.sendall(reply)
            except OSError:
                return

    # ------------------------------------------------------------ the parser

    def parse(self, buf):
        """The whole ParseCommand, as a pure function on the command bytes."""
        out = bytearray()
        cur = 0
        size = len(buf)
        fail = struct.pack("<IB", 5, IPC_FAIL)

        def string(s):
            raw = s.encode("utf-8")
            out.extend(struct.pack("<I", len(raw) + 1))
            out.extend(raw)
            out.append(0)

        while cur < size:
            op = buf[cur]
            cur += 1

            if op in (MSG_READ8, MSG_READ16, MSG_READ32, MSG_READ64):
                width = {MSG_READ8: 1, MSG_READ16: 2, MSG_READ32: 4, MSG_READ64: 8}[op]
                if cur + 4 > size:
                    return fail
                addr = struct.unpack_from("<I", buf, cur)[0]
                cur += 4
                if not self._valid(addr, width):
                    return fail
                # big-endian in guest memory, logical value little-endian out
                value = int.from_bytes(self.peek(addr, width), "big")
                out.extend(value.to_bytes(width, "little"))

            elif op in (MSG_WRITE8, MSG_WRITE16, MSG_WRITE32, MSG_WRITE64):
                width = {MSG_WRITE8: 1, MSG_WRITE16: 2,
                         MSG_WRITE32: 4, MSG_WRITE64: 8}[op]
                if cur + 4 + width > size:
                    return fail
                addr = struct.unpack_from("<I", buf, cur)[0]
                value = int.from_bytes(buf[cur + 4:cur + 4 + width], "little")
                cur += 4 + width
                if not self._valid(addr, width):
                    return fail
                self.poke(addr, value.to_bytes(width, "big"))

            elif op == MSG_STATUS:
                out.extend(struct.pack("<I", self.status))
                cur += 4          # the quirk: no argument, cursor moves anyway

            elif op == MSG_ID:
                string(self.title_id)
            elif op == MSG_TITLE:
                string(self.title)
            elif op == MSG_VERSION:
                string(self.version)
            elif op == MSG_UUID:
                string("fake-uuid")
            elif op == MSG_GAMEVERSION:
                string("01.00")
            else:
                return fail

        return struct.pack("<IB", 5 + len(out), IPC_OK) + bytes(out)

    # --------------------------------------------------------------- teardown

    def stop(self):
        self._stop = True
        try:
            self._sock.close()
        except OSError:
            pass
        self._thread.join(timeout=2.0)


def main():
    ap = argparse.ArgumentParser(description="a fake RPCS3 PINE server")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--id", default="NPEA00385")
    args = ap.parse_args()

    srv = FakePine(port=args.port)
    srv.boot(args.id)
    print("fake-pine listening on 127.0.0.1:%d as %s" % (srv.port, args.id))
    try:
        while True:
            srv._thread.join(1.0)
    except KeyboardInterrupt:
        srv.stop()


if __name__ == "__main__":
    main()
