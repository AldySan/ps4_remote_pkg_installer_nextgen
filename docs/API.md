# Remote PKG Installer NextGen — HTTP API Reference

The app serves two APIs on port **12800** (configurable at build time):

- **Legacy API** (`/api/...`) — 100% compatible with the original
  [Remote Package Installer](https://github.com/flatz/ps4_remote_pkg_installer).
  Every existing tool, web frontend and script keeps working unmodified.
- **v2 API** (`/api/v2/...`) — a cleaner interface with job management,
  progress with derived speed/percent, storage and logs.

All requests are HTTP/1.1. Legacy endpoints are `POST` with JSON bodies.
Responses are JSON with `Access-Control-Allow-Origin: *` and
`Connection: close` (one request per connection, like the original).

---

## Legacy API

### `POST /api/install`

Installs a package. Two request types:

```json
{ "type": "direct", "packages": ["http://192.168.1.10:8080/game.pkg"] }
```

Multi-part packages list every piece in order (piece 0 first):

```json
{ "type": "direct", "packages": ["http://host/piece0.pkg", "http://host/piece1.pkg"] }
```

Reference manifests (e.g. produced by *PS4 PKG Linker*):

```json
{ "type": "ref_pkg_url", "url": "http://192.168.1.10:8080/game.json" }
```

Success (task id `-1` means the application is already installed):

```json
{ "status": "success", "task_id": 42, "title": "GAME NAME" }
```

Failures return either `{"status": "fail", "error": "message"}` (HTTP 500)
or `{"status": "fail", "error_code": "0x80990088"}` (HTTP 200), matching
the original wire format.

> **Transfer acceleration** — pieces are streamed through the app's
> internal proxy with up to 4 parallel HTTP Range connections when the
> source server supports ranges and the piece is at least 8 MB. No client
> changes needed.

### `POST /api/get_task_progress`

```json
{ "task_id": 42 }
```

```json
{
  "status": "success",
  "bits": "0x0", "error": "0",
  "length": "0x0", "transferred": "0x0",
  "length_total": "0x1A2B3C00", "transferred_total": "0x1234000",
  "num_index": "0", "num_total": "1",
  "rest_sec": "120", "rest_sec_total": "120",
  "preparing_percent": "100", "local_copy_percent": "0"
}
```

(Lengths are hex strings; counters/percent are decimal — identical to the
original.)

### Task control

`POST /api/start_task` / `stop_task` / `pause_task` / `resume_task` /
`unregister_task` — all take `{ "task_id": N }` and return
`{"status": "success"}` or `{"status": "fail", "error_code": "0x..."}`.

### `POST /api/find_task`

```json
{ "content_id": "UP0001-CUSA09311_00-GAME000000000000", "sub_type": 6 }
```

Returns `{ "status": "success", "task_id": 42 }`. `sub_type` follows the
BGFT sub types (6 = game, 7 = additional content, 8 = patch, ...).

### `POST /api/is_exists`

```json
{ "title_id": "CUSA09311" }
```

```json
{ "status": "success", "exists": "true", "size": "0x1A2B3C00" }
```

`size` (hex string, installed size in bytes) is only present when the
application exists.

### Uninstall endpoints

| Endpoint | Body | Effect |
|---|---|---|
| `POST /api/uninstall_game` | `{ "title_id": "CUSA09311" }` | uninstall app |
| `POST /api/uninstall_patch` | `{ "title_id": "CUSA09311" }` | uninstall patch |
| `POST /api/uninstall_ac` | `{ "content_id": "UP0001-...-LABEL" }` | uninstall additional content |
| `POST /api/uninstall_theme` | `{ "content_id": "UP0001-...-LABEL" }` | uninstall theme |

All return `{"status": "success"}` or `{"status": "fail", "error_code": ...}`.

### `GET /static/<file>`

Serves the temporary manifests and icons under `/data` (content types:
`.json` → `application/json`, `.png` → `image/png`). Used internally by
BGFT. `..` and `:/` are rejected.

---

## v2 API

### `GET /api/v2/system`

```json
{
  "status": "success",
  "data": {
    "name": "Remote PKG Installer NextGen",
    "version": "0.1.0",
    "title_id": "RPIN00001",
    "ip": "192.168.1.20",
    "port": 12800,
    "network": true,
    "storage": { "free": 274877906944, "total": 500107862016 },
    "active_jobs": 2
  }
}
```

### `POST /api/v2/install`

```json
{ "url": "http://192.168.1.10:8080/game.pkg" }
```

or `{ "urls": ["...piece0.pkg", "...piece1.pkg"] }` or
`{ "ref_pkg_url": "http://.../game.json" }`.

```json
{
  "status": "success",
  "data": {
    "job_id": 1, "task_id": 42, "title": "GAME NAME",
    "content_id": "UP0001-CUSA09311_00-GAME000000000000",
    "package_size": 439715840
  }
}
```

### `GET /api/v2/jobs`

```json
{
  "status": "success",
  "data": {
    "jobs": [
      {
        "id": 1, "bgft_task_id": 42, "state": "running",
        "title": "GAME NAME", "content_id": "UP0001-...",
        "package_size": 439715840,
        "transferred": 138412032, "length_total": 439715840,
        "percent": 31, "speed_bps": 11258991,
        "is_patch": false, "error": 0,
        "rest_sec": 27, "num_index": 0, "num_total": 1
      }
    ]
  }
}
```

States: `pending`, `preparing`, `registered`, `running`, `paused`,
`completed`, `already_installed`, `failed`, `stopped`.

### `GET /api/v2/jobs/<id>`

Single job (same shape as the list entries).

### `POST /api/v2/jobs/<id>/pause` | `/resume` | `/stop`

Per-item transfer control (same actions as the on-screen dashboard
buttons). Returns `{"status": "success"}` or
`{"status": "fail", "error_code": ...}`.

### `GET /api/v2/storage`

```json
{
  "status": "success",
  "data": { "free": 274877906944, "total": 500107862016,
            "free_hr": "256.00 GB", "total_hr": "465.76 GB" }
}
```

### `GET /api/v2/logs`

Last 64 log entries (level + message):

```json
{ "status": "success", "data": { "logs": ["INFO install registered: ..."] } }
```

---

## Wire-format notes for legacy compatibility

- Success bodies use spaces after colons (`{ "status": "success" }`) —
  exactly like the original.
- Error bodies: parameter problems return HTTP 500 with
  `{ "status": "fail", "error": "message" }`; BGFT-level failures return
  HTTP 200 with `{ "status": "fail", "error_code": "0x........" }`.
- `is_exists` returns `"exists"` as a string and the size as a hex string,
  matching the original.
- Already-installed installs report success with `task_id: -1`.
- Every response carries `Access-Control-Allow-Origin: *` and
  `Connection: close`.
