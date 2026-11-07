# icmon — native Linux client for XM/iCSee IP cameras (work in progress)

Target camera: IPG-X6C-WEQ2 (XM530V200, firmware V5.04.R02). Priority feature: browse and play the
recordings on the camera's microSD **over the network, without removing the card**.

## Status

| Part | State |
|---|---|
| Protocol library `src/xm` (DVRIP login, file listing with paging, window download, frame demuxer) | written, unit-tested on synthetic data; login/listing/download/demux logic follows what was observed on the real camera |
| `xmarchive list / fetch` console tool | tested end to end against the **test-only** mock; run on the real camera by the user: list (266 files), fetch, window across two files: OK |
| `xmplay` (network playback through GStreamer, test tool) | built with GStreamer 1.24.2 on the user's Mint and played a 20 s window with picture and sound, no visible glitches (one run; no seeking/pause) |
| GTK4 app `icmon`: connect, calendar, 24-hour archive timeline | built and run by the user on Mint (GTK4): lists 288 recordings of a day, timeline OK; GTK-free timeline logic unit-tested |
| `icmon` playback: click on the timeline plays the camera's archive in the window (pause/resume/stop) | built and run by the user (before the window rework below): picture and sound, instant jumps between recordings, proportions OK, pause/resume after 10 s and 30+ s OK, stop then a new click plays, window can be closed during playback, no visible glitch at 5-minute file joins |
| Window rework: tabs Live View / Playback; side panel with the camera list (+ / - above it, settings icon per camera), below it the control module (Live) or calendar + the day's recordings (Playback); layouts 1/2/4; double click on a tile opens Playback; transport buttons (previous / play-pause / stop / next recording) centred under the picture, timeline under them | live view, camera list, add/edit dialog: **built and run by the user** (picture of two cameras side by side in the 2-layout confirmed). The side-panel rework, file list, transport buttons and removal with confirmation: **written, not yet built by the user** |
| Search for cameras on the network (UDP broadcast, port 34569) | **written, not tried on a real network**: wire format taken from the open-source `dvrip` project's documentation and an excerpt of its code; unit tests use a synthetic reply |
| Control module: PTZ, saved positions, light | PTZ and saved positions are written (next rows); the light button is written but its command is **unverified**. Motion tracking was removed from the panel (not implemented) |
| Live view over RTSP (GStreamer rtspsrc, no audio, no automatic reconnect) | **written, not yet tried on a real camera**; the default RTSP address is the form from the user's own working mpv script (rtsp://user:password@host:554/channel=1), which has no stream number |
| Passwords in the system keyring (libsecret, optional at build time) | **written, not yet tried**; without libsecret the password is kept in memory until the program closes |
| Latest UI batches: camera list fixed at 6 rows, start/stop icon on the selected camera, icon bar on the selected tile (save frame at the camera's full resolution, sound button muted by default, volume slider always visible), live picture scaled with lanczos at a fixed height (1080; 720 in the 4-camera layout), streams keep running and keep their picture when the layout changes, automatic restart (max 5 in a row) only after a stream that had been playing dropped, timeline edge to edge with rounded track and handle, ruler above the bar, time popup at the pointer, sound button + volume under the Playback picture, Settings window (folder for saved media: `<folder>/picture`; `<folder>/video` is reserved, recording download is not implemented) | **written, not yet built by the user** (GTK code only syntax-checked here against hand-made stubs) |
| Yellow dots in the calendar (days with recordings: one OPFileQuery per day of the month, background thread, cached per month) | **written, not yet tried**: the protocol call is unit-tested against the mock; that GTK draws the dot via the `:checked` state of `calendar label.day-number` is taken from the GTK source, not seen on screen |
| Live sound | **written, not tried**: the sound branch is always in the stream, muted by default; the button unmutes without reconnecting; sound comes from the selected tile only. Whether this camera sends audio over RTSP is not verified |
| HD/SD button | **removed**: the default RTSP address has no `{stream}` placeholder, so it showed the same picture. The sub stream is still used when the camera setting "sub stream when several cameras" is on and the address has `{stream}` |
| Round 4 (written, **not built by the user**; GTK code only syntax-checked against stubs): title bar in two parts as in GNOME Disks (program name + restart button above the side panel, only the selected camera's name on the right with the grid menu; titles in the system size; the separator lies on the side panel's line); side sections as own foldable blocks (arrow at the right, air above and below the sub-title, sub-titles aligned with "Cameras"); the recordings list stays in the side panel, the download button sits in the right of its title (downloads the selected recording; press again to stop and keep the part received as `...-partial.mkv`); saved positions as buttons (left click go, right click delete, "+" saves the current position under the next free number; the list is kept per camera in cameras.json, because the camera cannot list its own); Motion tracking switch removed; click on the timeline scrolls the list to the clip; the time label is left-aligned with the picture; Name / Address captions inside the bordered table with column separators; folder chooser = GtkFileDialog (GTK 4.10+) or GtkFileChooserNative (GTK 4.6, desktop portal); PTZ stop is sent no earlier than 250 ms after the start; calendar scan result and errors are shown in the status line and on stderr (`icmon calendar: ...`). Round 5 (**not built by the user**; player.cpp and downloader.cpp could not even be syntax-checked here: no gst-app headers): restart + grid buttons at the left of the right title part; left title part and side column share one width (size group); folded sections hide completely so the ones below move up; "Saved positions" heading with + (popover: number, "already saved on the camera" / "save the current position here") and - (confirmation, forgets in the list only); fixed 36 px square position buttons; restart button per camera row; switch "Reverse PTZ left / right" in the camera dialog; light button shows a sun (on) or a moon (off); playback speed 1x/2x/4x forward (video time stamps divided by the rate, sound off above 1x; **backward speeds -2x/-4x are NOT implemented**); play icon now follows the player after a click on the timeline. Calendar: the real cause was found from the user's log: the camera answers Ret=119 for a day that has not come yet, which ended the whole scan; a refused day is now skipped. Download: the muxer refused the audio caps given through the appsrc "caps" property (`not-negotiated`, reproduced with synthetic HEVC + A-law data; with a capsfilter the .mkv is valid: ffprobe shows hevc + pcm_alaw) |
| PTZ (hold a pad button to move, release to stop; zoom; saved positions: go to / save as number) | **written, not tried**: message 1400 and the `OPPTZControl` JSON follow the open-source DVRIP clients (python-dvr, dvrip-rs); the JSON is unit-tested only against that format. The user reported that another program's manual PTZ buttons do not work on this camera while presets do, so manual movement may fail here too. A safety stop is sent after 6 s |
| Light button on the active tile | **written, not tried**: reads `Camera.WhiteLight` (config messages 1042/1040), turns the light on by writing `WorkMode = "KeepOpen"` and writes the earlier mode back when switched off. The name `Camera.WhiteLight`, the reply shape and the value `"KeepOpen"` are NOT verified on this camera; if the program is closed with the light on, the camera stays in that mode. The camera's own lighting-mode setting (starlight / infrared / dual light) is not implemented: the mapping of those three modes to configuration values is unknown |
| Download of one recording to `<media folder>/video/<camera>-<date>-<time>.mkv` (button in the title of the recordings list) | **written, not tried**: HEVC + A-law muxed by GStreamer (matroskamux) from the verified archive download; whether the .mkv plays and its audio is in sync is unverified |  The stall at about 8 MB was most likely the single thread blocking in the video push while the muxer waited for audio; the pipeline now has unlimited appsrc/queue buffers (**not yet tried on the camera**).
| Motion tracking, playback speeds, timeline zoom, language selection | **not started** |

Nothing in the "not started" row is implemented; do not assume it works.

## Build and test

    sudo apt install build-essential cmake ninja-build      # or use make
    sudo apt install libgtk-4-dev                           # only for the `icmon` GUI
    sudo apt install libsecret-1-dev                        # optional: save camera passwords in the keyring
    cmake -S . -B build -G Ninja && cmake --build build
    cd build && ctest --output-on-failure

## Use

    export CAM_PASS='...'            # or you get a hidden prompt
    ./build/xmarchive list  --host 192.168.0.101 --user shield --date 2026-10-31
    ./build/xmarchive fetch --host 192.168.0.101 --user shield --start "2026-10-31 00:52:30" --secs 20 --out clip
    ffplay -f hevc clip.h265

Times are the camera's own clock. `fetch` starts the download `--lead` seconds (default 6) earlier and
drops frames until the first I-frame, because decoding can only start there. A window may cross into the next
5-minute file (checked once on the real camera, one case). Ctrl-C cancels and closes the connection.

    ./build/ICmon                      # GUI: address, user, password, Connect; pick a day; timeline shows recordings
    ./build/xmplay --host 192.168.0.101 --user shield --start "2026-10-31 21:30:30" --secs 20
    # needs: sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev (+ gstreamer1.0-libav, plugins-good/bad)

## Security notes

* DVRIP login sends the Sofia hash of the password **unencrypted**; anyone on the network path can
  replay it. Use only on a trusted LAN. RTSP uses Digest; ONVIF uses WS-Security digest.
* The password is read from `CAM_PASS` or a hidden prompt, is not stored and not printed.
* Repeated wrong logins lock the camera account (Ret=205). The tool never retries a failed login.

## What the tests prove — and do not

`tests/mock_camera.h` is a **test-only** model of the behaviour observed on one camera. Passing tests show
that our client handles those packets (64-entry pages, 32 KiB data packets, window starting mid-frame,
data packets that begin with `{`, empty packet as end, cancel). They say nothing about other firmware.

### Tests that need the real camera (not automated)

1. `xmarchive list` for a day with more than 64 files returns every file once (paging against real data).
2. `xmarchive fetch` for several windows: start of a file, end of a file (a window crossing two files was checked once: OK).
3. Resulting `.h265` plays; I-frame times match the requested time.
4. Audio: `.alaw` plays as G.711 A-law 8 kHz and stays in sync (checked by ear once).
5. Wrong password gives Ret=203 (seen); lockout Ret=205 (seen).
5a. Live view: the RTSP address form (default is unverified), tcp transport, one and several cameras at once, whether the camera limits simultaneous sessions, CPU load of 2 and 4 main streams (use the sub stream option if too heavy), behaviour when the camera is switched off (the tile must show an error, no automatic retry).
5c. Search: the camera answers the broadcast (port 34569) and the reply has the fields described in `src/app/discovery.h`; cameras on another subnet are not found.
5d. Picture quality: compare the live tile with mpv on the fence lines again (lanczos, fixed 1080 px height). Saved frames are the decoded camera frame (2304x2592 expected), PNG.
5e. Stream drops: the tile restarts the stream up to 5 times after a drop (message "Connection lost, reconnecting"); the log line on stderr `icmon live: stream error: ...` names the failing element. Cause of the earlier "Internal data stream error" with mpv/cayenue running in parallel is NOT established (changing the frame size of a running stream was removed as a suspect; the camera's IR-cut day/night switch is another).
5g. Sound: does the camera send audio over RTSP (its settings show G711 8 kHz), does the button unmute without reconnecting, does Playback sound work through the button.
5f. Calendar dots: appear after the day's list is read; check they match the days that really have recordings and move with the month buttons.
5b. Keyring: password saved, read back after a restart, removed when the camera is deleted.
6. Behaviour on other file tags than `[R]` (motion/alarm), on a second channel, after a camera reboot.

## Known unknowns

Meaning of the 4 unknown bytes in I-frame headers, audio codec byte, the F9 "info" frame length beyond the
first one, FileLength unit, keepalive message id. Seen in the GUI: the frame (2304x2592) holds TWO stacked pictures (two lenses); a view selector (top/bottom/both) is not implemented yet.


## Round 6 (written, not built by the user; GTK code syntax-checked against stubs only)
Restart button only on the selected camera row; saved positions in a 6-column grid of 36 px squares; header buttons start at the video's left edge; window buttons follow the system `gtk-decoration-layout` (also when it changes); calendar marks = lighter background + 1 px border; speed slider (double click = 1x) left of the transport buttons, time right of them; date before the time in the recordings list; download name `<camera>_DD.MM.YYYY_HH.MM.mkv`; language choice in Settings (System / English / Українська / Русский, applied after a restart; catalogs `po/ru.po`, `po/uk.po`, compiled to `locale/` by `tools/make_catalogs.py`, which needs no gettext tools; found next to the executable at `build/locale`). A few long technical messages stay English.

## Round 7 (written, not built by the user)
Program name `ICmon` (`APP_NAME` in `i18n.h`, never translated); tile gap 8 px; selected calendar day in the accent colour; saved-position squares forced to 36 px by CSS (`.preset-btn`, themes give text buttons ~64 px), 7 per row, no focus frame; "+" popover without the "already saved" button (note: saving overwrites the camera's own position with that number); camera presets cannot be listed (no known command, not invented); time label next to the sound button; Settings: start maximized switch, narrower language box; saved frames `<camera>_DD.MM.YYYY_HH.MM.SS.png`; left title part and left column fixed to the same width; a download that received clearly less than the recording's length is saved as `...-incomplete.mkv` with a message (cause of the earlier 2-minute file not known); CSS `overflow` (unsupported in GTK 4.6) replaced by `gtk_widget_set_overflow`.

## Round 8 (written, not built by the user)
Program is `ICmon` everywhere visible: binary `build/ICmon`, log prefixes, app id, default media folder `~/Downloads/ICmon_media`, config folder `~/.config/ICmon`. Saved positions: 4 squares per row (60 px, number as child label, no focus frame); Settings window has a fixed size; the time label sits next to the speed slider.

## Round 9
- Saved positions: grid of 4 equal columns, buttons fill the cell at the theme's height, Tab focus returns (outline drawn inside). Up to 16 positions; an existing number offers the next free one (else the previous); Ctrl+R stores the camera's current position again under the selected one (SetPreset on an existing number — unverified on the camera).
- Settings window fits its content. Playback: time "now / segment" beside the volume button; mouse wheel zooms the video, drag pans, double click resets; download stops running playback first.
- Camera-side preset list: no verified DVRIP command is known, so the list stays local (cameras.json). Not implemented by design; needs a capture of iCSee traffic on TCP 34567.

## Round 10
- Saving a position is local only (nothing is sent to the camera; Ctrl+R too). Goto still sends GotoPreset.
- Playback: zoom removed; original video resolution (no downscale); download keeps running when the day changes; sound slider no longer moves under the mouse (fixed-width time label); Date/Time headings in the recordings list; settings window opens without a height jump; focus outline of the position buttons drawn inside.

## Round 11
Focus frame of the position buttons removed (buttons are not focusable); Save position commits a typed number; plain system font in the Add-camera table header; time label fixed width (28 chars) without ellipsis; settings hints have fixed size; malloc_trim after live/playback stop (memory growth after restarts is NOT proven fixed, no profiler run).

## Round 12
Positions are stored in the camera again (SetPreset on add and Ctrl+R). Ret=119 for a day = empty day, not an error. Recordings list is a table (frame, Date | Time). Sound device buffer 80 ms (volume change is heard sooner). mallopt at start (mmap threshold 1 MB, 2 arenas) against heap growth over camera restarts - not measured. Settings window is resizable again (fixed-size variant jumped).
- Round 13: playback frames capped to 1296 px height (full size stuttered the UI).

## Round 14
Slow motion -4x/-2x (stretched time stamps, forward; unverified on camera); Saved positions block separate from Controls, not foldable, focus back; dialogs: buttons follow the bottom edge; no column separator in the recordings rows.
- Round 15: exit watchdog (4 s) against a process left after closing.

## Round 16 — camera-side presets over ONVIF (src/ptz/)
- `ptz_onvif` (SOAP/HTTP, WS-Security digest with the camera's own clock, plain-text fallback; no new libraries), `ptz_manager` (async refresh, cache, delete), `ptz_preset`.
- Flow: probe ports 8899, 80, 8080, 2020 (GetSystemDateAndTime) -> GetCapabilities (media/PTZ addresses) -> GetProfiles (ALL profiles are logged; only those with a PTZConfiguration are asked) -> GetPresets per such profile; the first non-empty answer is used. Names, tokens and PTZ positions go to stderr ("ICmon ONVIF (...)").
- UI: Saved positions are replaced by the camera's list (plain-number tokens only; other tokens are logged and skipped). Read at start, on camera selection, on the reconnect button, 1.5 s after "+" and after delete. If the camera answers with an EMPTY list while positions exist here, the list is kept (not wiped) and the status says so — see the log then.
- "−" now deletes in the camera: ONVIF RemovePreset with the token the camera reported (after confirmation); a number the camera did not report is cleared with the DVRIP ClearPreset.
- Creation (DVRIP SetPreset) and movement are unchanged. ASSUMPTION (unverified): the ONVIF preset token equals the DVRIP preset number. Tested only against a fake device in tests/test_onvif.cpp.
- Also: camera dialog scrolls (max 560 px) and has a minimum width; camera order (buttons under the list, Ctrl+Up/Down, drag one video window onto another); keyboard: Ctrl+1/2 tabs, F5 reconnect; playback controls row shrinks (window can be made narrower, snapping to half screen needs min width < 960); PTZ pad focus frame room.

## Round 17
A saved position counts as saved only when the camera lists it (re-read 1.5 s after SetPreset). If it is missing, the ONVIF SetPreset (token = number) is tried once, then the status says whether the camera confirmed. DVRIP preset commands and their answers are logged ('ICmon DVRIP ...'). Observed on the real camera: tokens 1,2,3,100, names equal tokens, identical on all 3 profiles.

## Round 18
4 x 4 grid (slot per camera, list sorted by slot, empty tiles skipped, new camera = first empty tile); drag a tile to any tile (no text under the pointer); double click = camera alone (again = back); speed slider smooth with snapping; camera dialog: wheel only on clicked number fields, height follows content up to 640 px, vertical PTZ reverse.

## Round 19
Tile selection frame follows the camera in the 4x4 grid (it used the list index); moved camera stays selected; status dots keep their colour after the list is rebuilt; the list order changes only between active cameras (a drop on an empty tile changes the tile only); camera dialog: more space, never lower than the folded state; volume slider 110 px again.

## Round 20
Camera dialog: switch spacing back, folded height remembered; timeline handle = upside-down drop; 2x2/1x2 grid: a tile can be dropped on an empty tile (camera moves to that place of its page); Ctrl+Q quits; privacy mode: right click on an active tile blurs it (CSS filter: blur, session only).

## Round 21
Blur removed. Grids 1x2 / 2x2 / 4x4: a camera sits on the tile of its slot (page = slot / tiles, the page of the selected camera is shown); drop on an empty tile moves only the tile, on an active one the two trade list places and tiles. Camera dialog: tabs Main / Additional (live view RTSP), no expander/scroller. Timeline drop not clipped (bar top 14 px).

## Round 22
1x2 / 2x2 are list pages again: a drag swaps only two active tiles (4x4 keeps the slot grid). Tabs styled like the system settings (stretched, dark rounded bar, blue underline) in the camera dialog; general settings has two tabs, General / About (version, GTK / GStreamer runtime versions). Camera list: icons 10 px from the edge, selected camera bold, its long name scrolls.
Round 22c: tabs = plain GtkStackSwitcher + GtkStack (no custom CSS, as JKMONs); 4x4 drop on an empty tile moves the camera there again (list order kept); 1x2/2x2 swap only active tiles.
Round 23: auto grid on adding a camera (1/2/4/16, only grows), double click in the list = camera alone, delete/save restart only that camera stream (no stop_all), slots compacted after delete, tab focus outline off, no preselected text in settings dialogs.

## Round 25
Icon (data/icons/hicolor/scalable/apps/icmon.svg, found next to the executable or in share/icons), .desktop file, install rules, CPack DEB; packaging/build-deb.sh and packaging/build-appimage.sh (linuxdeploy + GTK/GStreamer plugins). About tab: GitHub link (placeholder URL, ICMON_WEBSITE in settings_dialog.cpp). Camera tiles: 1px faint border, empty tiles shaded with a translucent text colour; header bars meet in a straight line.

## Round 26
Everything is named ICmon / icmon now: namespace, CMake targets, ICMON_* macros, gettext domain (icmon.mo), keyring schema org.icmon.Camera, config folder ~/.config/ICmon only (the old icam folder and saved passwords are not read: add the cameras again).

AppImage packaging was dropped (not maintained); the .deb and the normal build remain.
