[![REUSE status](https://api.reuse.software/badge/github.com/Immersive-Data-Center-Management/idtx-core)](https://api.reuse.software/info/github.com/Immersive-Data-Center-Management/idtx-core)

# IDTX Core

## About this project

Central component for immersive, multi-user interaction with Immersive Digital Twin Experience applications. This backend server enables the following use-cases in the context of the **IDTX Toolbox**

1. Serving USD files for download and import into other DCC tools like Godot. The `IDTX-Flow` add-on for Godot fully integrates this backend if remote USD import is used.
Other tools like the Asset Administration Shell can store stable urls to the hosted USD files to reference 3D asset data for the digital twins.

2. Serving USD files for multi-user concurrent authoring. 

3. Providing a multi-user collaboration runtime for immersive digital twin centered experiences

> Please note: At the current state of implementation only the use-case 1 is fully supported. The use-case 2 and 3 are WIP.

## Requirements and Setup

After check-out of the project the local build is setup to run SCons python scripts to donwload and build the required dependencies as well as the final binary. The following components are required to be installed before running the build scripts:

### All Operating Systems

- **Python3**, once installed use `pip` to install `scons`, `jinja2` and `pyside6`
- **Godot4.5**, to be able to test the plugin within a Godot project. The Godot version need to match the version of the [C++ bindings](https://github.com/godotengine/godot-cpp) used as a dependency.

### Windows

- **C++ Buildtools** - MS Visual Studio Build tools
- **CMake** - usually part of the MS Visual Studio Build tools

### MacOS

 - **C++ Buildtools** - The full Xcode IDE is required because OpenUSD's build script uses xcodebuild for codesigning,
 - **CMake** - use `brew install cmake` for example.

 Once all required software and tools are installed and configured the plugin can be build with the following command, executed at the root folder of this repository.

```bash
checked_out_repo_dir $>scons
```

> Please be patient, as the first initial build will download and compile openUSD from source. Depending on the used hardware this may take up to 40 minutes or more.

## Running the backend

Once the binary has been compiled the backend server can be started in the `bin/` folder. As the server uses an OIDC complient IDP to enable OAuth2.0 JWT authentication the corresponding configuration is required to be provided as environment variables. On local builds this can be achieved by putting a `.env` file next to the executable in the `bin/` folder with the following contents:

```env
OIDC_WELLKNOWN_URL=<usually something like: https://my-idp-server.com/.well-known/openid-configuration>
OIDC_AUDIENCES=

OAUTH_TOKEN_URL=<usually something like: https://my-idp-server.com/openid-connect/token>
OAUTH_CLIENT_ID=
```

For local testing without authentication, set `IDTX_INSECURE_DISABLE_AUTH=true` or `1`. When authentication is disabled, `OIDC_WELLKNOWN_URL` and `OIDC_AUDIENCES` are not required. Authentication remains enabled by default; do not set this insecure option in production.

The default port of the server is `8080`. The `.env` setting `SERVER_PORT=<other port>` allows to override this default setting.

### Building and Running the Containerized Version

To build and run the backend within a container, the `Dockerfile` of the repository can be used to build the image using a container runtime like Docker. Once build, run the image. The image exposes the port `8080`. When running the container version ensure the afforementioned environment is passed into the container.

## Build and Run Tests

The project uses `doctests` to run automated tests for the server functionality. To build the test runner execute:
```bash
checked_out_repo_dir $>scons tests=1
```

In addition to the `idtx-core` executable this will generate the `idtx-core-tests` executable in the `./bin` folder. Run this executable will execute all defined tests contained in the `tests/` folder.

### Load testing the websocket protocol

`tools/ws_load.py` simulates clients of one collaborative session against a running server and reports ack latency, broadcast fan-out latency, `queue_full` rejections and `server_seq` ordering. The `drag` scenario sends updates at a fixed rate (like a user dragging an object), the `burst` scenario sends a fixed number of updates as fast as possible:
```bash
checked_out_repo_dir $>python -m venv .venv
checked_out_repo_dir $>.venv/Scripts/pip install -r tools/requirements.txt
checked_out_repo_dir $>.venv/Scripts/python tools/ws_load.py drag --draggers 2 --observers 4 --rate 120 --duration 10
checked_out_repo_dir $>.venv/Scripts/python tools/ws_load.py burst --draggers 1 --count 5000
```
(On Linux/macOS use `.venv/bin/` instead of `.venv/Scripts/`.) The script generates its protobuf modules with the `protoc` of the vcpkg install, uploads a small cube scene and deletes its session afterwards. Pass `--token` (or set `IDTX_TOKEN`) when authentication is enabled.

### Current Available Endpoints

| Path                               | Authentication | Methods | Description |
|------------------------------------|----------------|---------|-------------|
| `/api/v1/auth/login`               | No             | POST    | Expects a username and password to authenticate against the provided IDP and hand out a JWT to be used for endpoints requiring authentication. |
| `/api/v1/health`                   | No             | GET     | Health check endpoint |
| `/api/v1/files`                    | Yes            | GET     | Returns a list of files available in the servers `uploads` folder as JSON response |
| `/api/v1/download/<path>`          | Yes            | HEAD    | Return 200 if a valid file exists at the given path. |
| `/api/v1/download/<path>`          | Yes            | GET     | Returns the contents of the file addressed by `<path>` |
| `/api/v1/upload`                   | Yes            | POST    | Upload a USD file (`.usd`, `.usda`, `.usdc`, `.usdz`). See below. |
| `/api/v1/thumbnail/<path>`         | Yes            | HEAD    | Return 200 if a thumbnail image has been generated for the USD file at `<path>`. |
| `/api/v1/thumbnail/<path>`         | Yes            | GET     | Returns the thumbnail image (PNG) generated for the USD file at `<path>`. |
| `/api/v1/sessions`                 | Yes            | GET     | Returns a list of current active multi-user sessions |
| `/api/v1/sessions`                 | Yes            | POST    | Create a new multi-user session for a specific USD file. The file path is given as JSon request body like `{ "usd_file": "scenes/foo.usda" }`. |
| `/api/v1/sessions/<string>`        | Yes            | GET     | Retreive details for the given multi-user session |
| `/api/v1/sessions/<string>/commit` | Yes            | POST    | Commit session changes (single/collab-edit) to the original USD file, attached to the session. |
| `/api/v1/sessions/<string>`        | Yes            | DELETE  | Tear down a given multi-user session and "disconnect" all clients. |

#### Uploading USD files

The upload endpoint accepts a `multipart/form-data` body with the following parts:

| Part        | Required | Description |
|-------------|----------|-------------|
| `file`      | yes      | Binary payload of the USD file. |
| `path`      | no       | Target directory *relative* to the server's `uploads/` root. May be empty (uploads to the root) or nested (e.g. `scenes/props`). |
| `filename`  | no       | Explicit filename; overrides the filename in the `file` part's `Content-Disposition`. Must end with `.usd`, `.usda`, `.usdc` or `.usdz`. |
| `overwrite` | no       | `"true"` to allow overwriting an existing file, defaults to `false`. |

All paths and filenames are strictly validated against directory-traversal (`..`, encoded slashes, absolute paths, symlink escape) before any bytes are written. The uploaded file is streamed to a `.part` temp file first and only atomically renamed into place on success.

Example (curl):

```bash
curl -X POST http://localhost:8080/api/v1/upload \
     -H "Authorization: Bearer $JWT" \
     -F "file=@./cube.usda" \
     -F "path=scenes/props" \
     -F "overwrite=false"
```

Response (`201 Created`):

```json
{
  "filepath":  "scenes/props/cube.usda",
  "filename":  "cube.usda",
  "directory": "scenes/props",
  "size":      12345,
  "thumbnail": {
    "status": "queued",
    "path":   "scenes/props/thumbs/cube.png",
    "error":  ""
  }
}
```

Error responses share the common `{ "error": "<code>", "message": "<text>" }` shape:

| Status | `error`             | Meaning |
|--------|---------------------|---------|
| 400    | `invalid_request`   | Missing part, bad filename, non-USD extension, invalid encoding. |
| 403    | `forbidden_path`    | Resolved path is outside the uploads root. |
| 409    | `conflict`          | Target file exists and `overwrite` is not `true`. |
| 413    | `payload_too_large` | Body exceeds `IDTX_UPLOAD_MAX_BYTES` (default 500 MiB). |
| 500    | `internal_error`    | I/O error writing the file or preparing the target directory. |

#### Thumbnail generation

After a successful upload the server schedules an *asynchronous* thumbnail-generation job on a small background worker. The response therefore returns quickly with `"thumbnail.status": "queued"`; polling `HEAD /api/v1/thumbnail/<usd_path>` returns `200` once the image is ready.

Thumbnails are stored in a `thumbs/` sub-folder next to the USD file:

```
uploads/
└── scenes/props/
    ├── cube.usda
    └── thumbs/
        └── cube.png
```

The client always addresses the thumbnail by the **USD file path** (`GET /api/v1/thumbnail/scenes/props/cube.usda`) — the server maps it to `<dir>/thumbs/<stem>.<image_ext>` internally, so clients do not need to know the layout.

The default `PlaceholderThumbnailGenerator` produces a small deterministic PNG derived from USD layer metadata. It does not require an OpenUSD build with imaging enabled and works in the minimal container image shipped with this repository. Because the generator is behind an abstract `ThumbnailGenerator` interface (see `source/thumbnails/`), a future Hydra-based renderer can be plugged in without changes to `FileServingController` or the HTTP routes.

Relevant environment variables:

| Variable                   | Default        | Meaning |
|----------------------------|----------------|---------|
| `IDTX_UPLOAD_MAX_BYTES`    | `524288000`    | Maximum accepted upload size in bytes (500 MiB). |
| `IDTX_THUMBNAIL_ENABLED`   | `true`         | Set to `false`/`0` to disable thumbnail generation entirely. |
| `IDTX_THUMBNAIL_SIZE`      | `256`          | Pixel edge length of the generated thumbnail (square). |

#### Real-time session protocol

Clients join a session via a websocket at `/ws?sid=<session id>` and exchange binary `idtxcore.BaseMessage` protobuf frames (see `shared/proto_messages/`).

- **Server-authoritative order.** Every session processes its commands (transform updates, joins, commits, reloads) strictly one at a time, in the order the server received them. The resulting order is the same for all clients.
- **`server_seq`.** A per-session version counter, incremented once for every stage change the server applies. `TransformBroadcast`, `Ack` and `SnapshotComplete` carry the `server_seq` of the state they describe. The values a client receives never go down, but several messages can share one value (an update's `Ack` and its broadcasts, all frames of a join snapshot). `0` means "no state": it is used by messages that do not reflect state and by Acks of rejected updates (`queue_full`, `session_closing`); clients ignore it for ordering.
- **`request_id`.** Set by the client on each `TransformUpdate` (e.g. a per-connection counter) and echoed in the matching `Ack`. Each update gets exactly one `Ack`, which is sent after the broadcasts the update triggered. Broadcasts are not sent back to the originating client.
- **Ack errors.** `queue_full` (the session's command queue is full; the update was dropped), `session_closing` (the session is being torn down; the update was dropped) and `apply_failed` (the update could not be applied to the stage).
- **Join snapshot.** After the `Handshake`, a joining client receives one `TransformBroadcast` per prim with session overrides, followed by `SnapshotComplete`, all carrying the same `server_seq`. Live broadcasts can arrive before the snapshot; the snapshot frames supersede them.
- **Commit.** `POST /api/v1/sessions/<id>/commit` is queued behind all previously received updates. It answers `200` with `{"committed": true}`, `409 nothing_to_commit`, or `503 unavailable` when the session is shutting down.

| Variable               | Default          | Meaning                                                                                                                        |
|------------------------|------------------|--------------------------------------------------------------------------------------------------------------------------------|
| `IDTX_SESSION_WORKERS` | CPU cores, max 4 | Worker threads shared by all sessions to process session commands. A single session never uses more than one worker at a time. |

## Support, Feedback, Contributing

This project is open to feature requests/suggestions, bug reports etc. via [GitHub issues](https://github.com/Immersive-Data-Center-Management/idtx-core/issues). Contribution and feedback are encouraged and always welcome. For more information about how to contribute, the project structure, as well as additional contribution information, see our [Contribution Guidelines](CONTRIBUTING.md).

## Security / Disclosure
If you find any bug that may be a security problem, please follow our instructions at [in our security policy](https://github.com/Immersive-Data-Center-Management/idtx-core/security/policy) on how to report it. Please do not create GitHub issues for security-related doubts or problems.

## Code of Conduct

We as members, contributors, and leaders pledge to make participation in our community a harassment-free experience for everyone. By participating in this project, you agree to abide by its [Code of Conduct](https://github.com/Immersive-Data-Center-Management/.github/blob/main/CODE_OF_CONDUCT.md) at all times.

## Licensing

Copyright 2026 SAP SE or an SAP affiliate company and idtx-core contributors. Please see our [LICENSE](LICENSE) for copyright and license information. Detailed information including third-party components and their licensing/copyright information is available [via the REUSE tool](https://api.reuse.software/info/github.com/Immersive-Data-Center-Management/idtx-core).
