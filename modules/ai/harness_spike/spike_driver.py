"""Phase 0 spike driver for codex app-server (pinned rust-v0.145.0).

Speaks newline-delimited JSON-RPC (no "jsonrpc" field) over stdio to a spawned
`codex app-server` child, mirroring the topology the C++ CodexHarnessDriver will use.
Every frame in both directions is logged to logs/<test>_<timestamp>.jsonl so a
transcript of each test becomes reference material for the real driver.

Usage:
  python spike_driver.py handshake        # initialize + model/list, then exit
  python spike_driver.py turn             # trivial one-turn conversation
  python spike_driver.py interrupt        # long turn, interrupt after 3s, measure latency
  python spike_driver.py steer            # long turn, steer mid-flight
"""

import json
import os
import queue
import subprocess
import sys
import threading
import time
from pathlib import Path

SPIKE_DIR = Path(__file__).parent
CODEX_EXE = SPIKE_DIR / "bin" / "codex-x86_64-pc-windows-msvc.exe"
CODEX_HOME = SPIKE_DIR / "codex_home"
WORKSPACE = SPIKE_DIR / "workspace"
ENV_FILE = SPIKE_DIR.parent / ".env"
LOGS = SPIKE_DIR / "logs"


def load_env_keys():
    env = os.environ.copy()
    for line in ENV_FILE.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            env[k.strip()] = v.strip()
    env["CODEX_HOME"] = str(CODEX_HOME)
    env["LITELLM_SPIKE_KEY"] = "sk-aristotle-spike"
    return env


# Provider/model overrides, e.g.:
#   set SPIKE_PROVIDER=kimi_proxy & set SPIKE_MODEL=kimi-k2.6 & python spike_driver.py turn
SPIKE_PROVIDER = os.environ.get("SPIKE_PROVIDER")
SPIKE_MODEL = os.environ.get("SPIKE_MODEL")


class AppServer:
    def __init__(self, test_name):
        LOGS.mkdir(exist_ok=True)
        self.log_path = LOGS / f"{test_name}_{int(time.time())}.jsonl"
        self.log_file = open(self.log_path, "w", encoding="utf-8")
        self.proc = subprocess.Popen(
            [str(CODEX_EXE), "app-server"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=load_env_keys(),
            cwd=str(WORKSPACE),
            text=True,
            encoding="utf-8",
            bufsize=1,
        )
        self.inbox = queue.Queue()
        self.next_id = 0
        threading.Thread(target=self._read_stdout, daemon=True).start()
        threading.Thread(target=self._read_stderr, daemon=True).start()

    def _log(self, direction, payload):
        if isinstance(payload, dict) and isinstance(payload.get("params"), dict) \
                and "apiKey" in payload["params"]:
            payload = {**payload, "params": {**payload["params"], "apiKey": "<redacted>"}}
        self.log_file.write(json.dumps({"t": time.time(), "dir": direction, "msg": payload}) + "\n")
        self.log_file.flush()

    def _read_stdout(self):
        for line in self.proc.stdout:
            line = line.strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                msg = {"_unparsed": line}
            self._log("recv", msg)
            self.inbox.put(msg)

    def _read_stderr(self):
        for line in self.proc.stderr:
            self._log("stderr", line.rstrip())

    def send(self, method, params=None, *, request=True):
        msg = {"method": method}
        if params is not None:
            msg["params"] = params
        if request:
            self.next_id += 1
            msg["id"] = self.next_id
        self._log("send", msg)
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()
        return msg.get("id")

    def respond(self, req_id, result):
        msg = {"id": req_id, "result": result}
        self._log("send", msg)
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()

    def wait_response(self, req_id, timeout=60):
        """Wait for the response to req_id; auto-handle/collect everything else."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                msg = self.inbox.get(timeout=max(0.1, deadline - time.time()))
            except queue.Empty:
                break
            if msg.get("id") == req_id and ("result" in msg or "error" in msg):
                return msg
            self.handle_async(msg)
        raise TimeoutError(f"no response to request {req_id}")

    def handle_async(self, msg):
        """Server->client requests and notifications encountered while waiting."""
        if "method" in msg and "id" in msg:  # server request (e.g. approval, dynamic tool)
            if msg["method"] == "item/tool/call" or "dynamicTool" in msg["method"]:
                params = msg.get("params", {})
                print(f"  [server request] {msg['method']} tool={params.get('tool')} "
                      f"args={json.dumps(params.get('arguments'))[:120]}")
                self.respond(msg["id"], self.run_dynamic_tool(params))
            else:
                print(f"  [server request] {msg['method']} -> auto-decline")
                self.respond(msg["id"], {"decision": "decline"})
        elif "method" in msg:
            self.on_notification(msg)

    def run_dynamic_tool(self, params):
        if params.get("tool") == "capture_test_screenshot":
            from mcp_test_server import PNG_B64
            return {"success": True, "contentItems": [
                {"type": "inputText", "text": "Screenshot captured (96x96)."},
                {"type": "inputImage", "imageUrl": f"data:image/png;base64,{PNG_B64}"},
            ]}
        return {"success": False, "contentItems": [
            {"type": "inputText", "text": f"unknown tool {params.get('tool')}"}]}

    def on_notification(self, msg):
        method = msg.get("method", "?")
        params = msg.get("params", {})
        if method == "item/agentMessage/delta":
            sys.stdout.write(params.get("delta", ""))
            sys.stdout.flush()
        elif method in ("item/started", "item/completed"):
            item = params.get("item", {})
            print(f"\n  [{method}] type={item.get('type')} id={item.get('id')}")
        elif method.startswith("turn/"):
            print(f"\n  [{method}] {json.dumps(params)[:200]}")

    def drain_until(self, predicate, timeout=120):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                msg = self.inbox.get(timeout=max(0.1, deadline - time.time()))
            except queue.Empty:
                break
            if "method" in msg and "id" in msg:
                self.handle_async(msg)
            elif "method" in msg:
                self.on_notification(msg)
                if predicate(msg):
                    return msg
        raise TimeoutError("condition not met")

    def initialize(self):
        rid = self.send("initialize", {
            "clientInfo": {"name": "aristotle-spike", "title": "Aristotle Spike", "version": "0.0.1"},
            "capabilities": {"experimentalApi": True},
        })
        resp = self.wait_response(rid, timeout=20)
        self.send("initialized", request=False)
        self._login_with_api_key()
        return resp

    def _login_with_api_key(self):
        rid = self.send("account/read", {"refreshToken": False})
        resp = self.wait_response(rid, timeout=15)
        account = (resp.get("result") or {}).get("account")
        if account:
            print(f"  [auth] already logged in ({account.get('type', '?')})")
            return
        api_key = load_env_keys().get("OPENAI_API_KEY")
        if not api_key:
            raise RuntimeError("OPENAI_API_KEY not found in .env")
        rid = self.send("account/login/start", {"type": "apiKey", "apiKey": api_key})
        resp = self.wait_response(rid, timeout=20)
        if "error" in resp:
            raise RuntimeError(f"apiKey login failed: {resp['error']}")
        print("  [auth] logged in via apiKey")

    def close(self):
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.log_file.close()
        print(f"\n[log] {self.log_path}")


def test_handshake():
    s = AppServer("handshake")
    try:
        resp = s.initialize()
        print("initialize ->", json.dumps(resp.get("result", resp.get("error")), indent=2)[:1500])
        rid = s.send("model/list", {})
        resp = s.wait_response(rid)
        print("model/list ->", json.dumps(resp.get("result", resp.get("error")), indent=2)[:3000])
    finally:
        s.close()


def start_thread_and_turn(s, prompt, model=None):
    thread_params = {"cwd": str(WORKSPACE)}
    if SPIKE_PROVIDER:
        thread_params["modelProvider"] = SPIKE_PROVIDER
    if SPIKE_MODEL:
        thread_params["model"] = SPIKE_MODEL
    if os.environ.get("SPIKE_COLLAB_MODE"):
        thread_params["collaborationMode"] = {
            "mode": os.environ["SPIKE_COLLAB_MODE"],
            "settings": {},
        }
    rid = s.send("thread/start", thread_params)
    resp = s.wait_response(rid)
    print("thread/start ->", json.dumps(resp)[:400])
    result = resp.get("result", {})
    thread_id = result.get("threadId") or result.get("thread", {}).get("id")
    params = {
        "threadId": thread_id,
        "input": [{"type": "text", "text": prompt}],
    }
    if model or SPIKE_MODEL:
        params["model"] = model or SPIKE_MODEL
    rid = s.send("turn/start", params)
    return thread_id, rid


def test_turn():
    s = AppServer("turn")
    try:
        s.initialize()
        _, rid = start_thread_and_turn(s, "Reply with exactly the single word: ready")
        s.drain_until(lambda m: m.get("method") == "turn/completed", timeout=120)
        try:
            resp = s.wait_response(rid, timeout=10)
            print("turn/start response ->", json.dumps(resp)[:800])
        except TimeoutError:
            pass
    finally:
        s.close()


def test_interrupt():
    s = AppServer("interrupt")
    try:
        s.initialize()
        thread_id, _ = start_thread_and_turn(
            s, "Count from 1 to 500, one number per line, no tools, just output text.")
        started = s.drain_until(lambda m: m.get("method") == "turn/started", timeout=60)
        turn_id = started["params"]["turn"]["id"]
        time.sleep(3.0)
        t0 = time.time()
        s.send("turn/interrupt", {"threadId": thread_id, "turnId": turn_id})
        done = s.drain_until(
            lambda m: m.get("method") == "turn/completed", timeout=60)
        dt = time.time() - t0
        status = done.get("params", {}).get("turn", {}).get("status", "?")
        print(f"\ninterrupt latency: {dt:.2f}s, final status: {status}")
    finally:
        s.close()


def test_steer():
    s = AppServer("steer")
    try:
        s.initialize()
        thread_id, _ = start_thread_and_turn(
            s, "Write a haiku about the moon. Take your time and think first.")
        started = s.drain_until(lambda m: m.get("method") == "turn/started", timeout=60)
        turn_id = started["params"]["turn"]["id"]
        time.sleep(1.0)
        s.send("turn/steer", {
            "threadId": thread_id,
            "expectedTurnId": turn_id,
            "input": [{"type": "text", "text": "Actually make it about the sun instead."}],
        })
        s.drain_until(lambda m: m.get("method") == "turn/completed", timeout=360)
    finally:
        s.close()


DYNAMIC_TOOLS = [{
    "type": "function",
    "name": "capture_test_screenshot",
    "description": "Captures a screenshot of the test viewport and returns it as an image.",
    "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
}]


def test_dynamic():
    s = AppServer("dynamic")
    try:
        s.initialize()
        thread_params = {"cwd": str(WORKSPACE), "dynamicTools": DYNAMIC_TOOLS}
        if SPIKE_PROVIDER:
            thread_params["modelProvider"] = SPIKE_PROVIDER
        if SPIKE_MODEL:
            thread_params["model"] = SPIKE_MODEL
        rid = s.send("thread/start", thread_params)
        resp = s.wait_response(rid)
        print("thread/start ->", json.dumps(resp)[:300])
        thread_id = (resp.get("result", {}).get("threadId")
                     or resp.get("result", {}).get("thread", {}).get("id"))
        if not thread_id:
            print("thread/start FAILED:", json.dumps(resp)[:800])
            return
        s.send("turn/start", {
            "threadId": thread_id,
            "input": [{"type": "text", "text":
                       "Call the capture_test_screenshot tool, then describe precisely "
                       "what the returned image shows: colors, shapes, and their "
                       "positions. Do not guess — describe only what you see."}],
            **({"model": SPIKE_MODEL} if SPIKE_MODEL else {}),
        })
        s.drain_until(lambda m: m.get("method") == "turn/completed", timeout=240)
    finally:
        s.close()


def test_mcp():
    s = AppServer("mcp")
    try:
        s.initialize()
        _, _ = start_thread_and_turn(
            s,
            "Call the capture_test_screenshot tool from the aristotle_test server, "
            "then describe precisely what the returned image shows: colors, shapes, "
            "and their positions. Do not guess — describe only what you see.")
        s.drain_until(lambda m: m.get("method") == "turn/completed", timeout=180)
    finally:
        s.close()


if __name__ == "__main__":
    tests = {"handshake": test_handshake, "turn": test_turn,
             "interrupt": test_interrupt, "steer": test_steer, "mcp": test_mcp,
             "dynamic": test_dynamic}
    name = sys.argv[1] if len(sys.argv) > 1 else "handshake"
    if name not in tests:
        print(f"unknown test '{name}'; options: {', '.join(tests)}")
        sys.exit(1)
    tests[name]()
