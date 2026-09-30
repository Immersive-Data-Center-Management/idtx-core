# Tools

## Load testing the websocket protocol

`ws_load.py` simulates clients of one collaborative session against a running server and reports ack latency, broadcast fan-out latency, rejections (`queue_full`, `stale`), corrections, `server_seq` ordering and whether every client ends on the server state. Its clients follow the client rules of the [real-time session protocol](../README.md#real-time-session-protocol): they show their own update right away, apply every broadcast they receive and send the highest `server_seq` they have received as the base of each update.

Scenarios:
- `drag` - some clients ("draggers") send updates for one prim at a fixed rate, like a user dragging an object; every client, including passive observers, receives the resulting broadcasts.
- `burst` - every dragger sends a fixed number of updates as fast as possible without waiting for acks.

### Setup

Once, from the repository root:
```bash
checked_out_repo_dir $>python -m venv .venv
checked_out_repo_dir $>.venv/Scripts/pip install -r tools/requirements.txt
```
(On Linux/macOS use `.venv/bin/` instead of `.venv/Scripts/`.)

### Running

Start a server without authentication (local testing only), for example with `IDTX_INSECURE_DISABLE_AUTH=1` in the `.env` next to the executable, then run a scenario:
```bash
checked_out_repo_dir $>.venv/Scripts/python tools/ws_load.py drag --draggers 2 --observers 4 --rate 120 --duration 10
checked_out_repo_dir $>.venv/Scripts/python tools/ws_load.py burst --draggers 1 --count 5000
```

The script generates its protobuf modules with the `protoc` of the vcpkg install (or `--protoc`), uploads a small cube scene and deletes its session afterwards. Pass `--token` (or set `IDTX_TOKEN`) when authentication is enabled, `--port` (default `SERVER_PORT` or `8080`) for another port, and `--session-id` to use an existing session. Run `tools/ws_load.py --help` for all options.
