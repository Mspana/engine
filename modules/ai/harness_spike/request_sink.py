"""Local HTTP sink posing as a Responses API provider: dumps the request body
codex sends (tools array included) to logs/sink_request.json, returns 500."""

import json
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

OUT = Path(__file__).parent / "logs" / "sink_request.json"


class Sink(BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        try:
            OUT.write_text(json.dumps(json.loads(body), indent=2), encoding="utf-8")
            print(f"captured {len(body)} bytes -> {OUT}")
        except json.JSONDecodeError:
            OUT.with_suffix(".raw").write_bytes(body)
        self.send_response(500)
        self.end_headers()
        self.wfile.write(b'{"error": "sink"}')

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    print("sink listening on 127.0.0.1:4141")
    HTTPServer(("127.0.0.1", 4141), Sink).serve_forever()
