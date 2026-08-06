"""Loopback relay + guest client, for testing the engine's outbound transport.

Mirrors the semantics of relay/worker.js and relay/local_relay.mjs so the Phase 2
path can be exercised with no Node runtime and, critically, nothing exposed: the
relay binds 127.0.0.1 only.

Run the engine with:
    ARISTOTLE_REMOTE=relay
    ARISTOTLE_RELAY_URL=ws://127.0.0.1:8788/room/<roomid>?role=host

Usage:
    python modules/ai/remote/tools/relay_test.py --serve      # relay only
    python modules/ai/remote/tools/relay_test.py --guest ...  # relay + guest checks
"""

import argparse
import base64
import hashlib
import json
import os
import socket
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2e_test import (  # noqa: E402
    WS,
    Channel,
    PAIR_INFO,
    SESSION_INFO,
    DIR_PAIR,
    check,
    code_from_log,
    decode_pair_code,
    hkdf,
    nonce_for,
    raw_pub,
    PASS,
    FAIL,
)

from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey  # noqa: E402
from cryptography.hazmat.primitives.ciphers.aead import AESGCM  # noqa: E402

WS_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


# --------------------------------------------------------------------------
# Relay
# --------------------------------------------------------------------------


class Peer(threading.Thread):
    """One relay-side socket. Forwards frames verbatim to the room's other peer."""

    def __init__(self, conn, room, role, rooms, lock):
        super().__init__(daemon=True)
        self.conn = conn
        self.room = room
        self.role = role
        self.rooms = rooms
        self.lock = lock
        self.buf = b""
        self.alive = True

    def send_frame(self, opcode, payload):
        n = len(payload)
        header = bytes([0x80 | opcode])
        if n < 126:
            header += bytes([n])
        elif n <= 0xFFFF:
            header += bytes([126]) + struct.pack(">H", n)
        else:
            header += bytes([127]) + struct.pack(">Q", n)
        try:
            self.conn.sendall(header + payload)
        except OSError:
            self.alive = False

    def send_json(self, obj):
        self.send_frame(0x1, json.dumps(obj).encode())

    def peer(self):
        other = "guest" if self.role == "host" else "host"
        with self.lock:
            return self.rooms.get(self.room, {}).get(other)

    def run(self):
        try:
            while self.alive:
                frame = self._parse()
                if frame is None:
                    chunk = self.conn.recv(65536)
                    if not chunk:
                        break
                    self.buf += chunk
                    continue
                opcode, payload = frame
                if opcode == 0x8:
                    break
                if opcode == 0x9:
                    self.send_frame(0xA, payload)
                    continue
                if opcode == 0xA:
                    continue
                other = self.peer()
                if other:
                    other.send_frame(opcode, payload)  # verbatim, never inspected
        except OSError:
            pass
        finally:
            self._leave()

    def _leave(self):
        self.alive = False
        with self.lock:
            room = self.rooms.get(self.room, {})
            if room.get(self.role) is self:
                room.pop(self.role, None)
            other = room.get("guest" if self.role == "host" else "host")
        if other:
            other.send_json({"t": "peer_gone"})
        try:
            self.conn.close()
        except OSError:
            pass

    def _parse(self):
        if len(self.buf) < 2:
            return None
        b0, b1 = self.buf[0], self.buf[1]
        opcode = b0 & 0x0F
        masked = bool(b1 & 0x80)
        n = b1 & 0x7F
        cursor = 2
        if n == 126:
            if len(self.buf) < cursor + 2:
                return None
            n = struct.unpack(">H", self.buf[cursor : cursor + 2])[0]
            cursor += 2
        elif n == 127:
            if len(self.buf) < cursor + 8:
                return None
            n = struct.unpack(">Q", self.buf[cursor : cursor + 8])[0]
            cursor += 8
        mask = b""
        if masked:
            if len(self.buf) < cursor + 4:
                return None
            mask = self.buf[cursor : cursor + 4]
            cursor += 4
        if len(self.buf) < cursor + n:
            return None
        payload = self.buf[cursor : cursor + n]
        self.buf = self.buf[cursor + n :]
        if masked:
            payload = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        return opcode, payload


class Relay(threading.Thread):
    def __init__(self, port):
        super().__init__(daemon=True)
        self.port = port
        self.rooms = {}
        self.lock = threading.Lock()
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        # Loopback only: this must never be reachable from outside the machine.
        self.sock.bind(("127.0.0.1", port))
        self.sock.listen(8)

    def run(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            threading.Thread(target=self._handshake, args=(conn,), daemon=True).start()

    def _handshake(self, conn):
        buf = b""
        conn.settimeout(15)
        try:
            while b"\r\n\r\n" not in buf:
                chunk = conn.recv(4096)
                if not chunk:
                    conn.close()
                    return
                buf += chunk
        except OSError:
            conn.close()
            return

        head, rest = buf.split(b"\r\n\r\n", 1)
        lines = head.decode(errors="replace").split("\r\n")
        target = lines[0].split(" ")[1] if len(lines[0].split(" ")) > 1 else "/"
        headers = {}
        for line in lines[1:]:
            if ":" in line:
                k, v = line.split(":", 1)
                headers[k.strip().lower()] = v.strip()

        path, _, query = target.partition("?")
        parts = [p for p in path.split("/") if p]
        role = "host"
        for kv in query.split("&"):
            if kv.startswith("role="):
                role = kv[5:]
        if len(parts) != 2 or parts[0] != "room" or role not in ("host", "guest"):
            conn.sendall(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            conn.close()
            return
        room_id = parts[1]

        key = headers.get("sec-websocket-key", "")
        accept = base64.b64encode(hashlib.sha1(key.encode() + WS_GUID).digest()).decode()
        conn.sendall(
            (
                "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                f"Connection: Upgrade\r\nSec-WebSocket-Accept: {accept}\r\n\r\n"
            ).encode()
        )
        conn.settimeout(None)

        peer = Peer(conn, room_id, role, self.rooms, self.lock)
        peer.buf = rest
        with self.lock:
            room = self.rooms.setdefault(room_id, {})
            old = room.get(role)
            if old:
                old.alive = False
                try:
                    old.conn.close()
                except OSError:
                    pass
            room[role] = peer
            other = room.get("guest" if role == "host" else "host")

        peer.send_json({"t": "relay_hello", "peer_present": other is not None})
        if other:
            other.send_json({"t": "peer_here"})
        peer.start()


# --------------------------------------------------------------------------
# Guest-side checks (same protocol as the browser client)
# --------------------------------------------------------------------------


def run_guest(port, room, pair_code):
    print("\nRelay transport")
    ws = WS("127.0.0.1", port, path=f"/room/{room}?role=guest")

    opcode, payload = ws.recv()
    hello = json.loads(payload)
    check("relay greets the guest", hello.get("t") == "relay_hello", str(hello))
    check("engine is already present in the room", hello.get("peer_present") is True)

    dev_priv = X25519PrivateKey.generate()
    dev_pub = raw_pub(dev_priv)

    # Pairing, end to end through the relay.
    ws.send_text({"t": "pair_hello", "dev_pk": base64.b64encode(dev_pub).decode(), "name": "relay-guest"})
    opcode, payload = ws.recv()
    msg = json.loads(payload)
    if not check("pair_hello reaches the engine through the relay", msg.get("t") == "pair_ack", str(msg)):
        ws.close()
        return

    srv_pub = base64.b64decode(msg["srv_pk"])
    salt = base64.b64decode(msg["salt"])
    shared = dev_priv.exchange(X25519PublicKey.from_public_bytes(srv_pub))
    key = hkdf(shared + decode_pair_code(pair_code), salt, PAIR_INFO, 32)
    proof = AESGCM(key).encrypt(nonce_for(DIR_PAIR, 0), b"pair-confirm" + dev_pub + srv_pub, None)
    ws.send_text({"t": "pair_confirm", "proof": base64.b64encode(proof).decode()})
    opcode, payload = ws.recv()
    msg = json.loads(payload)
    if not check("pairing completes over the relay", msg.get("t") == "pair_ok", str(msg)):
        ws.close()
        return

    # Session handshake, on a fresh relay connection.
    ws.close()
    ws = WS("127.0.0.1", port, path=f"/room/{room}?role=guest")
    ws.recv()  # relay_hello

    eph = X25519PrivateKey.generate()
    nonce_c = os.urandom(32)
    ws.send_text(
        {
            "t": "hello",
            "dev_pk": base64.b64encode(dev_pub).decode(),
            "eph_pk": base64.b64encode(raw_pub(eph)).decode(),
            "nonce_c": base64.b64encode(nonce_c).decode(),
        }
    )
    opcode, payload = ws.recv()
    msg = json.loads(payload)
    if not check("session handshake accepted over the relay", msg.get("t") == "hello_ack", str(msg)):
        ws.close()
        return

    eph_s = X25519PublicKey.from_public_bytes(base64.b64decode(msg["eph_pk"]))
    nonce_s = base64.b64decode(msg["nonce_s"])
    srv_static = X25519PublicKey.from_public_bytes(srv_pub)
    ikm = (
        eph.exchange(eph_s)
        + eph.exchange(srv_static)
        + dev_priv.exchange(eph_s)
        + dev_priv.exchange(srv_static)
    )
    okm = hkdf(ikm, nonce_c + nonce_s, SESSION_INFO, 64)
    ch = Channel(okm[:32], okm[32:])

    ws.send(0x2, ch.seal({"t": "hello_verify"}))
    opcode, payload = ws.recv()
    check("encrypted channel established through the relay", ch.open(payload).get("t") == "ready")

    ws.send(0x2, ch.seal({"t": "get_state"}))
    opcode, payload = ws.recv()
    state = ch.open(payload)
    check("state received through the relay", state.get("t") == "state", str(state)[:80])
    check("relayed device is also view-only by default", state.get("can_control") is False)
    ws.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8788)
    ap.add_argument("--room", default="")
    ap.add_argument("--pair-code", default=os.environ.get("ARISTOTLE_PAIRING_CODE", ""))
    ap.add_argument(
        "--code-from",
        default="",
        help="read the pairing code out of a file that captured the engine's stdout",
    )
    ap.add_argument("--serve", action="store_true", help="run the relay only, and block")
    ap.add_argument(
        "--external-relay",
        action="store_true",
        help="connect as a guest to a relay that is already running on --port",
    )
    args = ap.parse_args()
    if args.code_from:
        args.pair_code = code_from_log(args.code_from)
        print(f"Pairing code read from {args.code_from}")

    if not args.external_relay:
        relay = Relay(args.port)
        relay.start()
        print(f"Relay listening on 127.0.0.1:{args.port} (loopback only — not exposed)")
    else:
        print(f"Using the relay already running on 127.0.0.1:{args.port}")

    if args.serve:
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            return 0

    if not args.room:
        print("--room is required unless --serve is used")
        return 2

    time.sleep(0.5)
    run_guest(args.port, args.room, args.pair_code)
    print(f"\n{len(PASS)} passed, {len(FAIL)} failed")
    for name in FAIL:
        print("  failed: " + name)
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
