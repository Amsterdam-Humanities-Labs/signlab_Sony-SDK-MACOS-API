# Sony Camera Remote SDK - macOS Web API

`fx30MultiRecord`: one macOS binary that finds every Sony FX30 on USB and starts/stops them all at once through a REST API and an embedded dashboard.

## What it does

- Links the **Sony Camera Remote SDK (CRSDK)**, enumerates the FX30 bodies on USB (product ID `0x0e10`) and connects to each in Remote mode.
- REST API: start/stop recording on all cameras; status per camera (ISO, shutter, aperture, WB, format, frame rate, recording state, battery, media time, overheat).
- Dashboard: a self-contained HTML page served from `/`.
- Downloads clips over Contents Transfer mode; saves/applies settings presets (JSON) across all bodies.
- Recovery: `POST /api/scan` rescans; `POST /api/reset` does an IOKit USB re-enumeration first.
- `simpleCli/app/` also holds Sony's focused CLI samples (connection, live view, FTP, lens info, properties) as SDK references.

## Where it runs

- macOS 12.1+ (IOKit/CoreFoundation, prebuilt `.dylib`s), on the DRS machine the cameras are plugged into.
- Binds a local HTTP port (default 8080). It is a LAN tool, not a hosted service.

## Status

Production (DRS recording sessions). No tests, no packaging, no supervision.

## How to run / deploy

Needs Xcode, CMake >= 3.21.7 (`brew install cmake autoconf automake libtool`).

```bash
cd simpleCli && mkdir build && cd build
cmake -GXcode ..
cmake --build . --config Release --target fx30MultiRecord
./Release/fx30MultiRecord --port 8080 --download-path /tmp/fx30_downloads
```

Open `http://localhost:8080`.

## Configuration

| Flag | Default | Meaning |
|---|---|---|
| `--port` | 8080 | HTTP port |
| `--download-path` | `/tmp/fx30_downloads` | Where downloads go |
| `--preset` | `fx30_preset.json` | Settings preset file (gitignored, rig-specific) |

No config file, no credentials. The HTTP server has **no authentication**: bind it only on a trusted network.

## REST API

| Method | Path | Does |
|---|---|---|
| GET | `/` | Dashboard |
| GET | `/api/status` | All camera states and properties |
| POST | `/api/start`, `/api/stop` | Start / stop recording on all cameras |
| POST | `/api/scan`, `/api/reset` | Rescan / USB reset and rescan |
| POST | `/api/format` | Format media, body `{"slot":1}` or `{"slot":2}` |
| POST | `/api/download` | Download files from all cameras |
| POST | `/api/set-download-path` | Body `{"path":"..."}` |
| POST | `/api/preset/save`, `/api/preset/apply` | Save / apply preset |
| GET | `/api/preset` | Current preset |

## Dependencies

- **CRSDK v2.01**: headers in `simpleCli/app/CRSDK/`, macOS dylibs in `simpleCli/external/crsdk/`, Sony's PDFs in the root.
- **cpp-httplib** (MIT), vendored as `simpleCli/app/httplib.h`.
- macOS IOKit and CoreFoundation (USB reset).
- Used by the signcollect stack: https://github.com/Amsterdam-Humanities-Labs/signlab_signcollect-stack

SDK call notes: [docs/sdk-patterns.md](docs/sdk-patterns.md).

## Licence

The bundled Sony SDK (headers, dylibs, PDFs) is Sony's and falls under Sony's Camera Remote SDK licence, not this repo's; OSS notices (libssh2, OpenSSL, OpenCV, libusbK, libusb, cpp-httplib) are in [NOTICE.md](NOTICE.md).
