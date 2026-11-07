# ICmon

A native Linux client for iCSee-compatible (XM / Xiongmai) IP cameras: live view, recording playback and download, PTZ control and saved positions. Written in C++17 with GTK4 and GStreamer.

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

> ICmon is an independent project. It is not affiliated with, endorsed by or supported by iCSee, Xiongmai or any camera vendor.

## Features

- **Live view** over RTSP, with sound, snapshots and the camera's night light (where the camera supports it).
- **Grids** of 1×1, 1×2, 2×2 and 4×4. The grid grows by itself when you add cameras; a double click on a camera (in the list or on its tile) shows it alone, and another double click returns to the previous grid. Cameras are reordered by dragging one active tile onto another or with Ctrl+↑ / Ctrl+↓.
- **Playback** of the camera's SD-card archive: calendar with marked days, timeline, speeds from −4× to 4× (slow motion at 0.25× and 0.5×), volume, and download of recordings.
- **PTZ**: direction and zoom buttons with an adjustable speed, optional reverse of the horizontal and vertical axes per camera.
- **Saved positions** are stored in the camera itself (read, saved and deleted on the camera through DVRIP, with ONVIF as the fallback), so the phone app and ICmon show the same list.
- **Passwords** are kept in the system keyring (libsecret), not in the config file.
- **Languages:** English, Ukrainian, Russian (Settings → Language; restart to apply).
- Follows the system theme (light and dark) and its window decorations.

## Protocols

| Purpose | Protocol |
|---|---|
| Live view | RTSP (TCP) |
| Archive: calendar, file list, playback, download; PTZ and saved positions; light | DVRIP / Sofia, TCP port 34567 |
| Saved positions (read, remove, fallback save) | ONVIF over HTTP (ports 8899, 80, 8080, 2020 are tried) |
| Finding cameras in the local network | UDP discovery |

## Tested hardware

ICmon was developed and tested against one camera: **IPG-X6C-WEQ2** (XM530V200 chip, dual lens, firmware 000807DD). Other XM-based cameras may work but have not been tried. Reports are welcome.

Known limitations:

- Saved positions assume that the ONVIF preset token equals the DVRIP preset number. That holds on the tested camera; it is an assumption for others.
- The camera's night light can switch on by the camera's own sensor in daylight; ICmon cannot prevent that.
- The camera answers an archive search for days without recordings with an error; ICmon shows such days as empty.
- Slow motion at −4× and −2× on very long segments depends on how fast the camera sends data over TCP.

## Install

### .deb (Debian, Ubuntu, Linux Mint)

```sh
packaging/build-deb.sh
sudo apt install ./dist/icmon_*_amd64.deb
```

### From source

Dependencies (Debian / Ubuntu / Mint):

```sh
sudo apt install build-essential cmake ninja-build pkg-config \
  libgtk-4-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libsecret-1-dev \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-libav
```

Build and run:

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/ICmon
```

Run the tests (they need no camera; the camera side is simulated by test-only code):

```sh
ctest --test-dir build
```

Without libsecret the program still builds, but passwords are not kept between runs.

## Usage

1. Open the camera dialog with **+** in the camera list, press **Search** or enter the address by hand (port 34567, user, password), then **Save**.
2. The first tab of the left bar is the live view, the second one is the archive.
3. In the live view, pick a grid with the button in the top bar. Hover a tile for its buttons: night light, snapshot, sound and volume.
4. In the archive, pick a day in the calendar and a recording in the list or on the timeline.

Keyboard shortcuts:

| Keys | Action |
|---|---|
| Ctrl+1 / Ctrl+2 | Live view / archive |
| Ctrl+↑ / Ctrl+↓ | Move the selected camera up / down in the list |
| F5 | Reconnect the selected camera |
| Ctrl+R | Store the selected saved position again |
| Ctrl+Q | Quit |

Settings (the gear in the left bar): the folder for pictures and video (`picture` and `video` subfolders), language, start maximized.

## Files

- Settings and the list of cameras: `~/.config/ICmon/`
- Pictures and recordings: the folder chosen in Settings (default `~/Downloads/ICmon_media`)
- Passwords: the system keyring (schema `org.icmon.Camera`)

## Project layout

```
src/app/     the GTK4 application and its GUI-free core (camera store, RTSP URLs, discovery)
src/xm/      the DVRIP / Sofia protocol client
src/ptz/     ONVIF client and the saved-positions manager
tests/       unit tests with a test-only fake camera
tools/       translation catalogs (python3 tools/make_catalogs.py) and command-line helpers
packaging/   .deb build script
docs/        DEVLOG.md: the development log
```

## Translations

Strings live in `tools/translations.py`. Add a language or fix a string there, then run `python3 tools/make_catalogs.py`; it writes `po/` and `locale/`.

## Security note

DVRIP uses the camera's own, weakly protected login. Keep cameras off the internet and use them inside a trusted network or behind a VPN.

## License

[MIT](LICENSE)
