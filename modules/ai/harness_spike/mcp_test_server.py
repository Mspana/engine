"""Minimal MCP stdio server for the spike: one tool that returns a PNG image.

Stands in for the editor's screenshot tools to verify the full chain:
codex -> MCP tools/call -> image content block -> model actually SEES the image.
The image is deliberately distinctive (red field, green horizontal stripe,
white square in the top-left corner) so the model can only describe it
correctly by looking at pixels, not by guessing from the tool name.

Pure stdlib (hand-rolled PNG via zlib) — no PIL dependency.
Per codex issue #10334: image content is returned WITHOUT structuredContent.
"""

import base64
import json
import struct
import sys
import zlib


def make_png():
    w, h = 96, 96
    rows = []
    for y in range(h):
        row = bytearray([0])  # filter byte
        for x in range(w):
            if x < 24 and y < 24:
                px = (255, 255, 255)      # white square, top-left
            elif 40 <= y < 56:
                px = (0, 200, 0)          # green horizontal stripe
            else:
                px = (220, 30, 30)        # red field
            row.extend(px)
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


PNG_B64 = base64.b64encode(make_png()).decode()

TOOLS = [{
    "name": "capture_test_screenshot",
    "description": "Captures a screenshot of the test viewport and returns it as an image.",
    "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
}]


def reply(msg_id, result):
    out = json.dumps({"jsonrpc": "2.0", "id": msg_id, "result": result})
    sys.stdout.write(out + "\n")
    sys.stdout.flush()


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        msg = json.loads(line)
        method, msg_id = msg.get("method"), msg.get("id")
        if method == "initialize":
            reply(msg_id, {
                "protocolVersion": msg.get("params", {}).get("protocolVersion", "2025-06-18"),
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "aristotle-spike-tools", "version": "0.0.1"},
            })
        elif method == "tools/list":
            reply(msg_id, {"tools": TOOLS})
        elif method == "tools/call":
            name = msg.get("params", {}).get("name")
            if name == "capture_test_screenshot":
                reply(msg_id, {"content": [
                    {"type": "text", "text": "Screenshot captured (96x96)."},
                    {"type": "image", "data": PNG_B64, "mimeType": "image/png"},
                ]})
            else:
                reply(msg_id, {"content": [{"type": "text", "text": f"unknown tool {name}"}],
                               "isError": True})
        elif msg_id is not None:
            # Unknown request: standard JSON-RPC "method not found" so clients
            # (codex probes resources/list etc.) treat it as cleanly unsupported.
            out = json.dumps({"jsonrpc": "2.0", "id": msg_id,
                              "error": {"code": -32601, "message": "method not found"}})
            sys.stdout.write(out + "\n")
            sys.stdout.flush()


if __name__ == "__main__":
    main()
