# Remote PKG Installer NextGen

A modern rewrite of flatz's [ps4_remote_pkg_installer](https://github.com/flatz/ps4_remote_pkg_installer)
for jailbroken PS4 consoles, built with the
[OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain).

Install PKG packages remotely over your network from any device — no USB drives —
with a live on-screen dashboard: active transfers with real-time speed and progress,
pause/resume/stop/re-transfer controls per item, storage status, and an activity log.

## Highlights

- **Live dashboard UI** (dark tech theme) — no more static splash screen
- **Per-item controls** — pause, resume, stop, and re-transfer each transfer
- **Stability first** — works with or without a network connection; the server
  starts automatically once a link is available, and the app never crashes offline
- **Faster transfers** — adaptive multi-segment HTTP downloads for staged installs
- **Legacy API compatible** — the original Web API is preserved 1:1, so existing
  tools (remote_installer, web frontends, scripts) keep working
- **New v2 API** — a cleaner REST interface with job management, progress, and logs
- **Community friendly** — plain C11, one Makefile, CI builds every push

## Requirements

- PS4 with HEN (or any kernel payload providing fPKG support + `/data` mount),
  same as the original application
- A device on the same network to send commands (PC, phone, ...)

## Building

Requires the [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain)
(`OO_PS4_TOOLCHAIN` env var), `clang`, `lld`, and `python3`.

```sh
export OO_PS4_TOOLCHAIN=/path/to/OpenOrbis/PS4Toolchain
make
```

The final package is `IV0000-RPIN00001_00-RPINEXTGEN000000.pkg`.

Every push and pull request is built by CI (see `.github/workflows/build.yml`);
grab the `.pkg` from the workflow artifacts.

## Usage

1. Install the package on your PS4 (HEN must be active).
2. Launch it — the dashboard shows the console's IP, storage and transfers.
3. Send installs from any device on the same network (see below). The
   app works with or without a network link: the server starts
   automatically once a link is available, and the app never exits on
   its own.
4. Transfers run through the system's BGFT download manager — they
   continue even after the app closes.

### On-screen controls

| Button | Action |
|---|---|
| D-pad up/down | Select transfer |
| X (cross) | Pause / resume the selected transfer |
| O (circle) | Stop the selected transfer |
| [ ] (square) | Re-transfer (re-run a completed/failed/stopped transfer) |
| OPTIONS | Exit |

### Quick start (from a PC)

```sh
# Check the server
curl http://<PS4-IP>:12800/api/v2/system

# Install a package (URL served by any web server on your network)
curl -v 'http://<PS4-IP>:12800/api/install' \
  --data '{"type":"direct","packages":["http://<PC-IP>:8080/UP0001-CUSA09311_00-GAME.pkg"]}'

# Watch progress (speed, percent, ETA)
curl 'http://<PS4-IP>:12800/api/v2/jobs'

# Pause / resume / stop a job
curl 'http://<PS4-IP>:12800/api/v2/jobs/1/pause' -X POST
curl 'http://<PS4-IP>:12800/api/v2/jobs/1/resume' -X POST
curl 'http://<PS4-IP>:12800/api/v2/jobs/1/stop' -X POST
```

The full API reference (legacy + v2) is documented in [docs/API.md](docs/API.md).

## Roadmap

- [x] Project skeleton, build system, CI
- [x] Core services (logging, network monitor, storage info, pkg/SFO parsing, HTTP client)
- [x] Transfer engine (BGFT wrappers, task manager, adaptive multi-segment prefetch)
- [x] HTTP server (legacy API 1:1, v2 API, /static/ + accelerated piece proxy)
- [x] Live on-screen dashboard (status strip, transfer cards, activity log, controller controls)
- [x] Docs & polish ([API reference](docs/API.md))

## Development

Host unit tests cover the portable core (util/SFO/pkg parsing) and run on
every push:

```sh
make test
```

Fixtures in `tests/data/` are generated from a real build (`param.sfo` and
the first 0x3000 bytes of a real package header).

Regenerate the embedded bitmap font after changing the generator:

```sh
python3 tools/gen_font.py   # requires Pillow
```

Project layout:

```
src/            application code (C11, no framework)
thirdparty/     vendored cJSON + dbglogger (MIT), custom libSceBgft stub
tools/          asset/font generators, host unit tests
tests/data/     unit test fixtures
docs/API.md     HTTP API reference (legacy + v2)
```

## Credits

- **flatz** — the original [Remote Package Installer](https://github.com/flatz/ps4_remote_pkg_installer)
- **OpenOrbis Team** — the OpenOrbis PS4 Toolchain and SDL2 port
- **DaveGamble** — [cJSON](https://github.com/DaveGamble/cJSON) (MIT)
- **bucanero** — [dbglogger](https://github.com/bucanero/dbglogger) (MIT)

## License

GPL-3.0 — see [LICENSE](LICENSE).
