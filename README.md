# signlab_Sony-SDK-MACOS-API

`fx30MultiRecord` is one macOS program that finds every Sony FX30 on USB. It starts and stops recording on all of them at once, through a REST API and a built-in dashboard.

## What it does

- Uses the Sony Camera Remote SDK (CRSDK). It finds the FX30 cameras on USB (product ID `0x0e10`) and connects to each one in Remote mode.
- REST API: start and stop recording on all cameras. Status per camera: ISO, shutter, aperture, white balance, format, frame rate, recording state, battery, media time and overheating.
- Dashboard: a self-contained HTML page served at `/`.
- Downloads clips in Contents Transfer mode. Saves settings presets as JSON and applies them to all cameras.
- Recovery: `POST /api/scan` scans again. `POST /api/reset` first resets the USB devices through IOKit.
- `simpleCli/app/` also holds Sony's CLI samples (connection, live view, FTP, lens info, properties) as SDK examples. `CMakeLists.txt` builds each of them as its own target.

## Where it runs

- macOS 12.1 or later (IOKit, CoreFoundation and prebuilt `.dylib` files), on DRS, the studio Mac the cameras are plugged into.
- Listens on `0.0.0.0:<port>` (default 8080). There is no authentication, so keep it on the LAN or tailnet. Never expose it to the internet.
- Its only remote caller is `fx30proxy.php` in signlab_camera-control, over Tailscale. Access control belongs in that proxy.

## Status

Production, used in DRS recording sessions. No tests, no packaging, no supervision.

## How to run / deploy

You need Xcode and CMake 3.21.7 or later (`brew install cmake autoconf automake libtool`).

```bash
cd simpleCli && mkdir build && cd build
cmake -GXcode ..
cmake --build . --config Release --target fx30MultiRecord
./Release/fx30MultiRecord --port 8080 --download-path /tmp/fx30_downloads
```

Then open `http://localhost:8080`.

## Configuration

| Flag | Default | Meaning |
|---|---|---|
| `--port` | 8080 | HTTP port |
| `--download-path` | `/tmp/fx30_downloads` | Folder for downloaded clips |
| `--preset` | `fx30_preset.json` | Settings preset file (in `.gitignore`, specific to each camera setup) |

There is no config file, no credentials and no authentication. Anyone who can reach the port can record, format the media or change the download path.

## Controller app (`pyqtController/`)

`fx30_controller.py` is the PyQt6 window the studio operators use. It starts `fx30MultiRecord`, shows the cameras, and syncs and verifies the clips. Copy `config.example.json` to `config.json` and start it with `./run.sh`.

The **Status** tab shows one light per studio part (green, orange, red, or grey for unknown), a one-line detail and, when it is not green, what to do and a "Hulp" button that opens the help page. The checks are not in this app: it runs `tools/health.py --json` from drs-pipeline every 60 seconds (30 second timeout) and draws the answer. A dot on the tab shows the overall status from the camera tab. If the program is missing, fails, hangs or prints something else than JSON, the tab says why and the rest of the app carries on.

Two optional keys in `config.json`:

| Key | Default | Meaning |
|---|---|---|
| `drs_dir` | `/Users/signlab/drs` | Folder of drs-pipeline. The app runs `<drs_dir>/tools/health.py`. |
| `health_command` | `["/usr/bin/python3", "<drs_dir>/tools/health.py", "--json"]` | The full command, as a list. Overrides `drs_dir`. |

The command must exit 0 and print:

```json
{"generated_at": "2026-10-07T14:02:11+02:00", "host": "signlabs-mini", "overall": "ok|warn|fail",
 "checks": [{"id": "research_drive", "title": "Research drive", "status": "ok|warn|fail|unknown",
             "detail": "mounted, 1.0 TB free", "action": "", "help_url": "https://..."}]}
```

A `qr_screen` check with status `unknown` is filled in by the app, because only the app knows whether the QR page is open.

Tests (no cameras needed): `QT_QPA_PLATFORM=offscreen python -m pytest pyqtController/test_status_tab.py`, in an environment with PyQt6 and pytest.

## REST API

| Method | Path | What it does |
|---|---|---|
| GET | `/` | Dashboard |
| GET | `/api/status` | State and properties of all cameras |
| POST | `/api/start`, `/api/stop` | Start or stop recording on all cameras |
| POST | `/api/scan`, `/api/reset` | Scan again, or reset USB and then scan |
| POST | `/api/format` | Format media. Body: `{"slot":1}` or `{"slot":2}` |
| POST | `/api/download` | Download files from all cameras |
| GET | `/api/files` | Placeholder. Returns a message that points to `/api/download` |
| POST | `/api/set-download-path` | Body: `{"path":"..."}` |
| POST | `/api/preset/save`, `/api/preset/apply` | Save or apply a preset |
| GET | `/api/preset` | Current preset |

## Dependencies

- CRSDK v2.01: headers in `simpleCli/app/CRSDK/`, macOS dylibs in `simpleCli/external/crsdk/`, Sony's PDFs in the repo root.
- cpp-httplib (MIT licence), included as `simpleCli/app/httplib.h`.
- macOS IOKit and CoreFoundation, for the USB reset.
- Part of the SignCollect stack: [signlab_signcollect-stack](https://github.com/Amsterdam-Humanities-Labs/signlab_signcollect-stack).

Notes on SDK calls: [docs/sdk-patterns.md](docs/sdk-patterns.md).

## Licence

The bundled Sony SDK (headers, dylibs and PDFs) belongs to Sony. Sony's Camera Remote SDK licence applies to it, not this repo's licence. [NOTICE.md](NOTICE.md) has the licence notices for libusb and cpp-httplib, and the notices copied from Sony's SDK readme (libssh2, OpenSSL, OpenCV, libusbK).
