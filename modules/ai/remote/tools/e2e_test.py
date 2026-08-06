"""End-to-end check of the remote-access server, over a real socket.

Speaks the same protocol the browser client does — WebSocket framing, pairing
handshake, session handshake, encrypted frames — so it validates the parts unit
tests cannot: the listener, HTTP serving, the upgrade, and both handshakes
against the live server.

Usage (the engine must already be running with ARISTOTLE_REMOTE=loopback):
    python modules/ai/remote/tools/e2e_test.py [--port 8420] [--pair-code CODE]

Requires: cryptography (pip install cryptography).
"""

import argparse
import base64
import hashlib
import json
import os
import re
import socket
import struct
import sys

from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives import hashes, serialization

WS_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

DIR_PAIR, DIR_C2S, DIR_S2C = 1, 2, 3
PAIR_INFO = b"aristotle-remote-pair-v1"
SESSION_INFO = b"aristotle-remote-session-v1"

PASS, FAIL = [], []


def check(name, ok, detail=""):
    (PASS if ok else FAIL).append(name)
    print(("  PASS  " if ok else "  FAIL  ") + name + (f"  ({detail})" if detail else ""))
    return ok


def raw_pub(priv):
    return priv.public_key().public_bytes(
        encoding=serialization.Encoding.Raw, format=serialization.PublicFormat.Raw
    )


def nonce_for(direction, counter):
    return bytes([0, 0, 0, direction]) + struct.pack(">Q", counter)


def hkdf(ikm, salt, info, length):
    return HKDF(algorithm=hashes.SHA256(), length=length, salt=salt, info=info).derive(ikm)


# --------------------------------------------------------------------------
# Minimal WebSocket client
# --------------------------------------------------------------------------


class WS:
    def __init__(self, host, port, path="/ws"):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.buf = b""
        key = base64.b64encode(os.urandom(16))
        req = (
            f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key.decode()}\r\nSec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(req.encode())

        while b"\r\n\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("connection closed during upgrade")
            self.buf += chunk
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        if b"101" not in head.split(b"\r\n")[0]:
            raise RuntimeError(f"upgrade refused: {head.split(chr(13).encode())[0]!r}")

        expected = base64.b64encode(hashlib.sha1(key + WS_GUID).digest()).decode()
        if expected.lower() not in head.decode(errors="replace").lower():
            raise RuntimeError("Sec-WebSocket-Accept mismatch")

    def send(self, opcode, payload):
        mask = os.urandom(4)
        n = len(payload)
        header = bytes([0x80 | opcode])
        if n < 126:
            header += bytes([0x80 | n])
        elif n <= 0xFFFF:
            header += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            header += bytes([0x80 | 127]) + struct.pack(">Q", n)
        masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self.sock.sendall(header + mask + masked)

    def send_text(self, obj):
        self.send(0x1, json.dumps(obj).encode())

    def recv(self, timeout=10):
        """Returns (opcode, payload). Server frames are never masked."""
        self.sock.settimeout(timeout)
        while True:
            frame = self._try_parse()
            if frame is not None:
                return frame
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("connection closed")
            self.buf += chunk

    def _try_parse(self):
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
        if masked:
            raise RuntimeError("server sent a masked frame (protocol violation)")
        if len(self.buf) < cursor + n:
            return None
        payload = self.buf[cursor : cursor + n]
        self.buf = self.buf[cursor + n :]
        return opcode, payload

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


# --------------------------------------------------------------------------
# Encrypted channel
# --------------------------------------------------------------------------


class Channel:
    def __init__(self, k_send, k_recv):
        self.send_key, self.recv_key = AESGCM(k_send), AESGCM(k_recv)
        self.send_counter = 0
        self.last_recv = -1

    def seal(self, obj):
        header = bytes([1, 1]) + struct.pack(">Q", self.send_counter)
        ct = self.send_key.encrypt(nonce_for(DIR_C2S, self.send_counter), json.dumps(obj).encode(), header)
        self.send_counter += 1
        return header + ct

    def open(self, frame):
        assert frame[0] == 1 and frame[1] == 1, "bad frame header"
        counter = struct.unpack(">Q", frame[2:10])[0]
        assert counter > self.last_recv, f"counter went backwards ({counter} <= {self.last_recv})"
        plain = self.recv_key.decrypt(nonce_for(DIR_S2C, counter), frame[10:], frame[:10])
        self.last_recv = counter
        return json.loads(plain)


# --------------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------------


def test_http(host, port):
    print("\nHTTP serving")
    s = socket.create_connection((host, port), timeout=10)
    s.sendall(f"GET / HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n".encode())
    data = b""
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
    s.close()
    head = data.split(b"\r\n\r\n", 1)[0].decode(errors="replace")
    check("serves the client page", data.startswith(b"HTTP/1.1 200"))
    check("declares a content security policy", "Content-Security-Policy" in head)
    check("page is non-empty HTML", b"<html" in data.lower(), f"{len(data)} bytes")

    s = socket.create_connection((host, port), timeout=10)
    s.sendall(f"GET /../secret HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n".encode())
    resp = s.recv(4096)
    s.close()
    check("rejects unknown/traversal paths", resp.startswith(b"HTTP/1.1 404"))


def decode_pair_code(code):
    """base64url -> bytes, tolerant of how the code reaches the shell.

    The engine prints `ARISTOTLE_PAIRING_CODE=<code>`, and that whole line is easy
    to copy by accident. Every character of the prefix is also a valid base64
    character, so a bad paste used to surface far away as "Incorrect padding".
    Catch it here and say what is wrong instead.
    """
    raw = (code or "").strip().strip("\"'")
    for prefix in ("ARISTOTLE_PAIRING_CODE=", "ARISTOTLE_PAIRING_CODE:"):
        if raw.upper().startswith(prefix):
            raw = raw[len(prefix):].strip()
    if not raw:
        raise SystemExit("No pairing code. Pass --pair-code or set ARISTOTLE_PAIRING_CODE.")

    bad = set(raw) - set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")
    if bad:
        # Show position and codepoint. A console font, a codepage, or a copy out
        # of a screenshot can all substitute a lookalike character without
        # changing the length, and the character name alone does not reveal that.
        marks = "\n".join(
            f"    index {i:>2}: {c!r}  U+{ord(c):04X}"
            for i, c in enumerate(raw)
            if c in bad
        )
        raise SystemExit(
            f"Pairing code has characters that are not base64url ({len(raw)} chars):\n"
            f"  got: {raw!r}\n"
            f"{marks}\n"
            "  hex: " + raw.encode("utf-8", "backslashreplace").hex() + "\n"
            "  Copy only the value after the '=' sign, or use --code-from <logfile>."
        )
    if len(raw) % 4 == 1:
        raise SystemExit(
            f"Pairing code is {len(raw)} characters, which cannot be base64. Expected 43.\n"
            f"  got: {raw!r}\n"
            "  The paste is probably truncated, or it kept part of the printed line."
        )
    if len(raw) != 43:
        print(f"  warning: pairing code is {len(raw)} characters; the engine prints 43")
    return base64.urlsafe_b64decode(raw + "=" * (-len(raw) % 4))


CODE_LINE_RE = re.compile(r"ARISTOTLE_PAIRING_CODE=([A-Za-z0-9_\-]{43})")


def code_from_log(path):
    """Pull the newest pairing code out of a file the engine wrote.

    Removes the human copy step, which is where the code keeps getting damaged.
    Point this at whatever captured the engine's stdout, for example:
        godot.exe 2>&1 | Tee-Object -FilePath pair.log
    """
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        raise SystemExit(f"Cannot read {path}: {e}")

    found = CODE_LINE_RE.findall(data.decode("utf-8", "replace"))
    if not found and b"\x00" in data:
        # Windows PowerShell 5.1's Tee-Object always writes UTF-16LE and has no
        # -Encoding parameter. The pattern is pure ASCII, so dropping the NUL
        # bytes is enough to find it in either UTF-16 byte order.
        found = CODE_LINE_RE.findall(data.replace(b"\x00", b"").decode("latin-1", "replace"))
    if not found:
        raise SystemExit(
            f"No ARISTOTLE_PAIRING_CODE= line in {path}.\n"
            "  Start the engine with ARISTOTLE_REMOTE_PAIR=1 and capture its output."
        )
    return found[-1]


def do_pairing(host, port, code, dev_priv):
    print("\nPairing handshake")
    ws = WS(host, port)
    dev_pub = raw_pub(dev_priv)
    ws.send_text({"t": "pair_hello", "dev_pk": base64.b64encode(dev_pub).decode(), "name": "e2e-test"})

    opcode, payload = ws.recv()
    msg = json.loads(payload)
    if msg.get("t") != "pair_ack":
        check("server accepts pair_hello", False, str(msg))
        ws.close()
        return None
    check("server accepts pair_hello", True)

    srv_pub = base64.b64decode(msg["srv_pk"])
    salt = base64.b64decode(msg["salt"])

    shared = dev_priv.exchange(X25519PublicKey.from_public_bytes(srv_pub))
    key = hkdf(shared + decode_pair_code(code), salt, PAIR_INFO, 32)
    proof = AESGCM(key).encrypt(nonce_for(DIR_PAIR, 0), b"pair-confirm" + dev_pub + srv_pub, None)
    ws.send_text({"t": "pair_confirm", "proof": base64.b64encode(proof).decode()})

    opcode, payload = ws.recv()
    msg = json.loads(payload)
    ok = check("pairing accepted with the right code", msg.get("t") == "pair_ok", str(msg))
    device_id = msg.get("device_id")
    ws.close()
    return (srv_pub, device_id) if ok else None


def test_bad_pairing(host, port):
    """A wrong code must fail even though the key exchange itself succeeds."""
    print("\nPairing with a wrong code")
    dev_priv = X25519PrivateKey.generate()
    dev_pub = raw_pub(dev_priv)
    ws = WS(host, port)
    ws.send_text({"t": "pair_hello", "dev_pk": base64.b64encode(dev_pub).decode(), "name": "impostor"})
    opcode, payload = ws.recv()
    msg = json.loads(payload)
    if msg.get("t") != "pair_ack":
        check("wrong pairing code is rejected", False, "no pair_ack")
        ws.close()
        return
    srv_pub = base64.b64decode(msg["srv_pk"])
    salt = base64.b64decode(msg["salt"])
    shared = dev_priv.exchange(X25519PublicKey.from_public_bytes(srv_pub))
    key = hkdf(shared + os.urandom(32), salt, PAIR_INFO, 32)  # wrong secret
    proof = AESGCM(key).encrypt(nonce_for(DIR_PAIR, 0), b"pair-confirm" + dev_pub + srv_pub, None)
    ws.send_text({"t": "pair_confirm", "proof": base64.b64encode(proof).decode()})
    try:
        opcode, payload = ws.recv(timeout=5)
        msg = json.loads(payload) if opcode == 0x1 else {"t": "close"}
        check("wrong pairing code is rejected", msg.get("t") != "pair_ok", str(msg.get("code", msg.get("t"))))
    except Exception as e:
        check("wrong pairing code is rejected", True, f"connection dropped: {type(e).__name__}")
    ws.close()


def test_session(host, port, dev_priv, srv_pub):
    print("\nSession handshake and commands")
    ws = WS(host, port)
    dev_pub = raw_pub(dev_priv)
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
    if msg.get("t") != "hello_ack":
        check("paired device is accepted", False, str(msg))
        ws.close()
        return
    check("paired device is accepted", True)

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
    ready = ch.open(payload)
    check("encrypted channel established", ready.get("t") == "ready", str(ready))

    ws.send(0x2, ch.seal({"t": "get_state"}))
    opcode, payload = ws.recv()
    state = ch.open(payload)
    check("state returned over the encrypted channel", state.get("t") == "state", str(state)[:90])
    check(
        "new device defaults to VIEW-ONLY",
        state.get("can_control") is False,
        f"can_control={state.get('can_control')}",
    )

    ws.send(0x2, ch.seal({"t": "get_history"}))
    opcode, payload = ws.recv()
    hist = ch.open(payload)
    check("history returned", hist.get("t") == "history", f"{len(hist.get('items', []))} items")

    ws.send(0x2, ch.seal({"t": "send", "text": "this must be refused"}))
    opcode, payload = ws.recv()
    refusal = ch.open(payload)
    check(
        "view-only device cannot send messages",
        refusal.get("t") == "error" and refusal.get("code") == "view_only",
        str(refusal.get("code")),
    )

    # Replay the last frame verbatim: the counter check must kill the session.
    replay = ch.seal({"t": "ping"})
    ws.send(0x2, replay)
    opcode, payload = ws.recv()
    check("ping answered", ch.open(payload).get("t") == "pong")
    ws.send(0x2, replay)  # same counter again
    try:
        opcode, payload = ws.recv(timeout=5)
        killed = opcode == 0x8
    except Exception:
        killed = True
    check("replayed frame terminates the session", killed)
    ws.close()


def test_unknown_device(host, port, srv_pub):
    print("\nRejections")
    ws = WS(host, port)
    stranger = X25519PrivateKey.generate()
    eph = X25519PrivateKey.generate()
    ws.send_text(
        {
            "t": "hello",
            "dev_pk": base64.b64encode(raw_pub(stranger)).decode(),
            "eph_pk": base64.b64encode(raw_pub(eph)).decode(),
            "nonce_c": base64.b64encode(os.urandom(32)).decode(),
        }
    )
    try:
        opcode, payload = ws.recv(timeout=5)
        msg = json.loads(payload) if opcode == 0x1 else {}
        check(
            "unpaired device is rejected",
            msg.get("t") == "error" and msg.get("code") == "unknown_device",
            str(msg.get("code")),
        )
    except Exception as e:
        check("unpaired device is rejected", True, f"dropped: {type(e).__name__}")
    ws.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8420)
    ap.add_argument("--pair-code", default=os.environ.get("ARISTOTLE_PAIRING_CODE", ""))
    ap.add_argument(
        "--code-from",
        default="",
        help="read the pairing code out of a file that captured the engine's stdout",
    )
    args = ap.parse_args()
    if args.code_from:
        args.pair_code = code_from_log(args.code_from)
        print(f"Pairing code read from {args.code_from}")

    print(f"Testing remote access at {args.host}:{args.port}")
    test_http(args.host, args.port)

    if not args.pair_code:
        print("\n(no pairing code supplied — skipping the handshake tests)")
    else:
        # Order matters: a successful pairing burns the code (single use), so
        # the wrong-code attempt has to happen while the window is still open.
        test_bad_pairing(args.host, args.port)
        dev_priv = X25519PrivateKey.generate()
        result = do_pairing(args.host, args.port, args.pair_code, dev_priv)
        if result:
            srv_pub, _ = result
            test_session(args.host, args.port, dev_priv, srv_pub)
            test_unknown_device(args.host, args.port, srv_pub)

    print(f"\n{len(PASS)} passed, {len(FAIL)} failed")
    if FAIL:
        for name in FAIL:
            print("  failed: " + name)
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
