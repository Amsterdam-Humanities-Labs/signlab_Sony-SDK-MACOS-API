# signlab_Sony-SDK-MACOS-API

`fx30MultiRecord` is one macOS program that finds every Sony FX30 on USB. It starts and stops recording on all of them at once, through a REST API and a built-in dashboard.

## What it does

- Uses the Sony Camera Remote SDK (CRSDK). It finds the FX30 cameras on USB (product ID `0x0e10`) and connects to each one in Remote mode.
- REST API: start and stop recording on all cameras. Status per camera: ISO, shutter, aperture, white balance, format, frame rate, recording state, battery, media time and overheating.
- Dashboard: a self-contained HTML page served at `/`.
- Downloads clips in Contents Transfer mode. Saves settings presets as JSON and applies them to all cameras.
- Recovery: `POST /api/scan` scans again. `POST /api/reset` first resets the USB devices through IOKit.
- `simpleCli/app/` also holds Sony's CLI samples (connection, live view, FTP, lens info, properties) as SDK examples.

## Where it runs

- macOS 12.1 or later (IOKit, CoreFoundation and prebuilt `.dylib` files), on DRS, the studio Mac the cameras are plugged into.
- Listens on `0.0.0.0:<port>` (default 8080). There is no authentication, so keep it on the LAN or tailnet. Never expose it to the internet.
- Its only remote caller is `fx30proxy.php` in signlab_studio_beta, over Tailscale. Access control belongs in that proxy.

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

The bundled Sony SDK (headers, dylibs and PDFs) belongs to Sony. Sony's Camera Remote SDK licence applies to it, not this repo's licence. Notices for the open-source components (libssh2, OpenSSL, OpenCV, libusbK, libusb, cpp-httplib) are in [NOTICE.md](NOTICE.md).
