#!/usr/bin/env python3
"""Websocket load and churn client for idtx-core.

Simulates clients of one collaborative session and reports how the server
keeps up: ack latency, broadcast fan-out latency, rejections (queue_full,
stale), corrections, server_seq ordering and whether every client ends on
the server state.

Clients behave as the protocol requires: they show their own update right
away, apply every broadcast they receive and send the highest server_seq they
have received as the base of each update.

Scenarios:
  drag   Some clients ("draggers") send TransformUpdates for one prim at a
         fixed rate, like a user dragging an object; every client, including
         passive observers, receives the resulting broadcasts.
  burst  Every dragger sends a fixed number of updates as fast as possible
         without waiting for acks.

Setup (once, from the repository root):
  python -m venv .venv
  .venv/Scripts/pip install -r tools/requirements.txt   (Windows)
  .venv/bin/pip install -r tools/requirements.txt       (Linux/macOS)

Run a server without authentication (local testing only), e.g.
  IDTX_INSECURE_DISABLE_AUTH=1 bin/idtx-core
and then, for example:
  .venv/Scripts/python tools/ws_load.py drag --draggers 2 --observers 4 --rate 120 --duration 10
  .venv/Scripts/python tools/ws_load.py burst --draggers 1 --count 5000

The protobuf modules are generated at startup from shared/proto_messages
with the protoc from the vcpkg install (or --protoc) into a temporary
directory. The script uploads a small cube scene and opens a new session on
it unless --session-id is given.
"""

from __future__ import annotations

import argparse
import asyncio
import glob
import importlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import uuid
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
PROTO_DIR = REPO_ROOT / "shared" / "proto_messages"

CUBE_USDA = """#usda 1.0
(
    defaultPrim = "Root"
)

def Xform "Root"
{
    def Xform "Cube"
    {
        double3 xformOp:translate = (0, 0, 0)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
}
"""

pb = None  # base_pb2, loaded by load_protos()


# ---------------------------------------------------------------------------
# Setup helpers
# ---------------------------------------------------------------------------

def find_protoc(explicit: str | None) -> str:
    if explicit:
        return explicit
    pattern = str(REPO_ROOT / "thirdparty" / "vcpkg_installed" / "*" / "tools" / "protobuf" / "protoc*")
    for candidate in sorted(glob.glob(pattern)):
        if os.access(candidate, os.X_OK):
            return candidate
    found = shutil.which("protoc")
    if found:
        return found
    sys.exit("protoc not found; build the project once or pass --protoc")


def load_protos(protoc: str, out_dir: str) -> None:
    global pb
    protos = [p.name for p in PROTO_DIR.glob("*.proto")]
    subprocess.run([protoc, f"-I{PROTO_DIR}", f"--python_out={out_dir}", *protos], check=True)
    sys.path.insert(0, out_dir)
    pb = importlib.import_module("base_pb2")


def http_request(method: str, url: str, token: str | None, body: bytes | None = None,
                 content_type: str | None = None) -> tuple[int, str]:
    req = urllib.request.Request(url, data=body, method=method)
    if content_type:
        req.add_header("Content-Type", content_type)
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return resp.status, resp.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")


def upload_scene(base_url: str, token: str | None, filename: str) -> None:
    boundary = uuid.uuid4().hex
    parts = []
    for name, value in (("filename", filename), ("path", ""), ("overwrite", "true")):
        parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n\r\n{value}\r\n')
    parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="{filename}"\r\n'
                 f"Content-Type: application/octet-stream\r\n\r\n")
    body = "".join(parts).encode() + CUBE_USDA.encode() + f"\r\n--{boundary}--\r\n".encode()
    status, text = http_request("POST", f"{base_url}/api/v1/upload", token, body,
                                f"multipart/form-data; boundary={boundary}")
    if status not in (200, 201):
        sys.exit(f"upload failed: HTTP {status} {text}")


def create_session(base_url: str, token: str | None, usd_file: str) -> str:
    body = json.dumps({"usd_file": usd_file, "mode": "collaborative_edit"}).encode()
    status, text = http_request("POST", f"{base_url}/api/v1/sessions", token, body, "application/json")
    if status != 201:
        sys.exit(f"session creation failed: HTTP {status} {text}")
    return json.loads(text)["session_id"]


# ---------------------------------------------------------------------------
# Clients
# ---------------------------------------------------------------------------

@dataclass
class Stats:
    """Everything one client observed."""
    sent: int = 0
    send_times: dict[int, float] = field(default_factory=dict)       # request_id -> t
    ack_latency: list[float] = field(default_factory=list)
    acks_ok: int = 0
    acks_rejected: dict[str, int] = field(default_factory=dict)
    ok_seqs: list[int] = field(default_factory=list)
    ok_values: list[tuple[int, float]] = field(default_factory=list)  # (server_seq, x)
    broadcasts: int = 0
    broadcast_seqs: list[int] = field(default_factory=list)
    shown: float | None = None  # x the client currently shows
    fanout_latency: list[float] = field(default_factory=list)
    last_broadcast_at: float = 0.0
    seq_regressions: int = 0
    unknown_request_ids: int = 0
    last_seq: int = 0


class Client:
    def __init__(self, index: int, url: str, sid: str, usd_file: str, prim: str,
                 token: str | None, value_times: dict[float, float]):
        self.index = index
        self.url = url
        self.sid = sid
        self.usd_file = usd_file
        self.prim = prim
        self.token = token
        self.value_times = value_times  # shared: x value -> send time
        self.stats = Stats()
        self.ws = None
        self.reader: asyncio.Task | None = None
        self.snapshot_done = asyncio.Event()
        self.acks_pending = 0
        self.all_acked = asyncio.Event()
        self.all_acked.set()

    async def connect(self) -> None:
        import websockets
        headers = {"Authorization": f"Bearer {self.token}"} if self.token else None
        self.ws = await websockets.connect(self.url, max_size=None, compression=None,
                                           additional_headers=headers, open_timeout=10)
        self.reader = asyncio.create_task(self._read_loop())
        await asyncio.wait_for(self.snapshot_done.wait(), 10)

    async def _read_loop(self) -> None:
        import websockets
        try:
            async for frame in self.ws:
                now = time.perf_counter()
                msg = pb.BaseMessage()
                msg.ParseFromString(frame)
                self._on_message(msg, now)
        except websockets.ConnectionClosed:
            pass

    def _on_message(self, msg, now: float) -> None:
        kind = msg.WhichOneof("message")
        st = self.stats
        if kind == "snapshot_complete":
            st.last_seq = max(st.last_seq, msg.server_seq)
            self.snapshot_done.set()
            return
        if not self.snapshot_done.is_set():
            return  # handshake and snapshot frames
        if kind in ("ack", "xform_broadcast"):
            if msg.server_seq and msg.server_seq < st.last_seq:
                st.seq_regressions += 1
            st.last_seq = max(st.last_seq, msg.server_seq)
        if kind == "ack":
            sent_at = st.send_times.pop(msg.request_id, None)
            if sent_at is None:
                st.unknown_request_ids += 1
                return
            st.ack_latency.append(now - sent_at)
            if msg.ack.ok:
                st.acks_ok += 1
                st.ok_seqs.append(msg.server_seq)
                st.ok_values.append((msg.server_seq, unique_x(self.index, msg.request_id)))
            else:
                st.acks_rejected[msg.ack.error] = st.acks_rejected.get(msg.ack.error, 0) + 1
            self.acks_pending -= 1
            if self.acks_pending == 0:
                self.all_acked.set()
        elif kind == "xform_broadcast":
            st.broadcasts += 1
            st.broadcast_seqs.append(msg.server_seq)
            st.last_broadcast_at = now
            x = msg.xform_broadcast.update.seperate.translation.x
            st.shown = x
            sent_at = self.value_times.get(x)
            if sent_at is not None:
                st.fanout_latency.append(now - sent_at)

    async def send_update(self, request_id: int, x: float) -> None:
        msg = pb.BaseMessage()
        msg.session_id = self.sid
        msg.request_id = request_id
        msg.server_seq = self.stats.last_seq  # the base: highest server_seq received
        upd = msg.xform_update
        upd.session_id = self.sid
        upd.usd_file = self.usd_file
        upd.prim_path = self.prim
        upd.timestamp = int(time.time() * 1000)
        upd.seperate.translation.x = x
        upd.seperate.scale.x = upd.seperate.scale.y = upd.seperate.scale.z = 1.0
        payload = msg.SerializeToString()
        now = time.perf_counter()
        self.stats.send_times[request_id] = now
        self.value_times[x] = now
        self.acks_pending += 1
        self.all_acked.clear()
        self.stats.sent += 1
        self.stats.shown = x
        await self.ws.send(payload)

    async def close(self) -> None:
        if self.ws is not None:
            await self.ws.close()
        if self.reader is not None:
            await self.reader


def unique_x(client_index: int, request_id: int) -> float:
    """A value no other update of the run uses, so a broadcast identifies its update."""
    return client_index * 1_000_000 + request_id + 0.5


async def drag(client: Client, rate: float, duration: float) -> float:
    """Send at a fixed rate; returns the achieved send rate."""
    interval = 1.0 / rate
    start = time.perf_counter()
    request_id = 0
    while True:
        target = start + request_id * interval
        if target - start >= duration:
            break
        delay = target - time.perf_counter()
        if delay > 0:
            await asyncio.sleep(delay)
        request_id += 1
        await client.send_update(request_id, unique_x(client.index, request_id))
    elapsed = time.perf_counter() - start
    return client.stats.sent / elapsed if elapsed > 0 else 0.0


async def burst(client: Client, count: int) -> float:
    start = time.perf_counter()
    for request_id in range(1, count + 1):
        await client.send_update(request_id, unique_x(client.index, request_id))
    elapsed = time.perf_counter() - start
    return count / elapsed if elapsed > 0 else 0.0


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

def pct(values: list[float], q: float) -> float:
    if not values:
        return float("nan")
    s = sorted(values)
    return s[min(len(s) - 1, int(q * len(s)))]


def fmt_ms(values: list[float]) -> str:
    if not values:
        return "n/a"
    ms = [v * 1000 for v in values]
    return (f"p50 {pct(ms, 0.50):7.2f} ms  p90 {pct(ms, 0.90):7.2f} ms  "
            f"p99 {pct(ms, 0.99):7.2f} ms  max {max(ms):7.2f} ms  (n={len(ms)})")


def report(args, clients: list[Client], draggers: list[Client], send_rates: list[float],
           send_end: float, run_start: float) -> None:
    total_sent = sum(c.stats.sent for c in draggers)
    total_ok = sum(c.stats.acks_ok for c in draggers)
    rejected: dict[str, int] = {}
    for c in draggers:
        for k, v in c.stats.acks_rejected.items():
            rejected[k] = rejected.get(k, 0) + v
    missing_acks = sum(len(c.stats.send_times) for c in draggers)

    print()
    print(f"scenario        : {args.scenario}, {len(draggers)} dragger(s), "
          f"{len(clients) - len(draggers)} observer(s)")
    if args.scenario == "drag":
        print(f"target rate     : {args.rate:.0f} Hz per dragger for {args.duration:.1f} s")
    print(f"achieved send   : " + ", ".join(f"{r:.0f}/s" for r in send_rates))
    print(f"updates sent    : {total_sent}")
    print(f"acks            : {total_ok} ok, "
          + (", ".join(f"{v} {k}" for k, v in sorted(rejected.items())) or "0 rejected")
          + f", {missing_acks} missing")
    print(f"ack latency     : {fmt_ms([v for c in draggers for v in c.stats.ack_latency])}")

    fanout = [v for c in clients for v in c.stats.fanout_latency]
    print(f"fan-out latency : {fmt_ms(fanout)}")

    # Every applied update is broadcast to every client except its sender.
    # Any other broadcast is a correction sent to one client only.
    applied = {s for d in draggers for s in d.stats.ok_seqs}
    expected = 0
    got = 0
    corrections = 0
    for c in clients:
        others_ok = sum(d.stats.acks_ok for d in draggers if d is not c)
        expected += others_ok
        got += sum(1 for s in c.stats.broadcast_seqs if s in applied)
        corrections += sum(1 for s in c.stats.broadcast_seqs if s not in applied)
    print(f"broadcasts      : {got} received / {expected} expected, {corrections} correction(s)")

    # The server state is the value of the update applied last.
    applied_values = [v for d in draggers for v in d.stats.ok_values]
    if applied_values:
        final_x = max(applied_values)[1]
        converged = sum(1 for c in clients if c.stats.shown == final_x)
        print(f"final state     : {converged}/{len(clients)} client(s) show the server state")

    last_bcast = max((c.stats.last_broadcast_at for c in clients), default=0.0)
    if last_bcast:
        print(f"drain after send: {max(0.0, last_bcast - send_end) * 1000:.1f} ms "
              "(last broadcast after the last send)")

    all_seqs = sorted(s for c in draggers for s in c.stats.ok_seqs)
    dup_seqs = len(all_seqs) - len(set(all_seqs))
    per_sender_order = all(all(a < b for a, b in zip(c.stats.ok_seqs, c.stats.ok_seqs[1:]))
                           for c in draggers)
    regressions = sum(c.stats.seq_regressions for c in clients)
    unknown = sum(c.stats.unknown_request_ids for c in clients)
    print(f"server_seq      : {'ok' if per_sender_order else 'NOT increasing'} per sender, "
          f"{dup_seqs} duplicate(s), {regressions} regression(s) seen by clients"
          + (f", {unknown} ack(s) with unknown request_id" if unknown else ""))
    if all_seqs:
        span = all_seqs[-1] - all_seqs[0] + 1
        print(f"                  {len(all_seqs)} applied updates over seq span {span}")
    print(f"wall time       : {time.perf_counter() - run_start:.2f} s")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

async def run(args) -> int:
    base_url = f"http://{args.host}:{args.port}"
    if args.session_id:
        sid = args.session_id
    else:
        upload_scene(base_url, args.token, args.usd_file)
        sid = create_session(base_url, args.token, args.usd_file)
    print(f"session         : {sid}")

    ws_url = f"ws://{args.host}:{args.port}/ws?sid={sid}"
    value_times: dict[float, float] = {}
    clients = [Client(i + 1, ws_url, sid, args.usd_file, args.prim, args.token, value_times)
               for i in range(args.draggers + args.observers)]
    await asyncio.gather(*(c.connect() for c in clients))
    draggers = clients[:args.draggers]

    run_start = time.perf_counter()
    if args.scenario == "drag":
        send_rates = await asyncio.gather(*(drag(c, args.rate, args.duration) for c in draggers))
    else:
        send_rates = await asyncio.gather(*(burst(c, args.count) for c in draggers))
    send_end = time.perf_counter()

    try:
        await asyncio.wait_for(asyncio.gather(*(c.all_acked.wait() for c in draggers)), args.settle)
    except asyncio.TimeoutError:
        print(f"warning: not all acks arrived within {args.settle:.0f} s")
    # Give the last broadcasts a moment to reach the observers.
    await asyncio.sleep(0.5)

    report(args, clients, draggers, list(send_rates), send_end, run_start)
    await asyncio.gather(*(c.close() for c in clients), return_exceptions=True)

    if not args.session_id and not args.keep_session:
        http_request("DELETE", f"{base_url}/api/v1/sessions/{sid}", args.token)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("scenario", choices=["drag", "burst"])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=int(os.environ.get("SERVER_PORT", "8080")))
    parser.add_argument("--token", default=os.environ.get("IDTX_TOKEN"),
                        help="bearer token when authentication is enabled (default: $IDTX_TOKEN)")
    parser.add_argument("--session-id", help="use an existing session instead of creating one")
    parser.add_argument("--keep-session", action="store_true", help="do not delete the created session")
    parser.add_argument("--usd-file", default="ws_load_cube.usda", help="scene uploaded and opened")
    parser.add_argument("--prim", default="/Root/Cube")
    parser.add_argument("--draggers", type=int, default=1, help="clients that send updates")
    parser.add_argument("--observers", type=int, default=2, help="clients that only receive")
    parser.add_argument("--rate", type=float, default=60.0, help="drag: updates per second per dragger")
    parser.add_argument("--duration", type=float, default=5.0, help="drag: seconds to send")
    parser.add_argument("--count", type=int, default=2000, help="burst: updates per dragger")
    parser.add_argument("--settle", type=float, default=30.0, help="seconds to wait for outstanding acks")
    parser.add_argument("--protoc", help="protoc executable (default: vcpkg install, then PATH)")
    args = parser.parse_args()
    if args.draggers < 1:
        parser.error("--draggers must be at least 1")

    with tempfile.TemporaryDirectory(prefix="idtx_ws_load_") as out_dir:
        load_protos(find_protoc(args.protoc), out_dir)
        return asyncio.run(run(args))


if __name__ == "__main__":
    sys.exit(main())
