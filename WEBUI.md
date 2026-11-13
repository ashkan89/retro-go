# Retro-Go control room

Enable **Web manager** in the launcher options, connect to the same Wi-Fi network,
and open the address shown on the device. Keep the launcher open while managing
it. The interface needs no Internet connection, external fonts, JavaScript
framework, cloud account, or SD-hosted HTML. It supports desktop and mobile
browsers, keyboard navigation, reduced motion, and light/dark appearance.

The manager includes:

- **Overview:** live SD free space, RAM, uptime, battery and connection status.
- **Game library:** installed ROM folders with shortcuts for each system.
- **Files & saves:** games, saves, BIOS, artwork, themes and music; breadcrumbs,
  filtering, sorting, pagination, downloads, new files/folders, rename/move and
  confirmed deletion of individual or selected entries.
- **Transfers:** drag/drop or multi-file selection, a sequential upload queue,
  progress and speed, cancellation, retries and confirmed replacement.
- **Device:** volume for the current audio output, screen brightness, health
  information and confirmed restart. Settings apply on the launcher thread.
- **Network:** SSID, address, signal, channel and station/access-point status.
  Change Wi-Fi on the console, then reconnect at its new address.
- **Firmware:** installed application inventory, image upload to
  `/retro-go/firmware`, log verbosity, session activity and a downloadable JSON
  diagnostic report. Select a staged image and request installation, then confirm
  on the console. The existing factory updater validates target and image integrity
  before installation. Uploading an image alone does not install it.

## Firmware and SD packaging

The page source is `launcher/main/webui/index.html`. CMake runs
`tools/pack_webui.py` to create a deterministic gzip byte array in the build
directory. It is linked as read-only data in the launcher app and served with
`Content-Encoding: gzip`. The packer enforces a 32 KiB compressed asset budget.
Changes to either the source or packer rebuild the asset automatically. No new
partition is required: all existing firmware image and launcher flash workflows
include the interface. Networking-disabled builds discard the unused asset.

Build a complete image in an ESP-IDF environment, replacing the target as needed:

```sh
python rg_tool.py --target esp32-p4-n16r32-st7796 --release-build build-img factory launcher retro-core prboom-go gwenesis fmsx retro-legacy gbsp
```

For an existing installation, build the launcher and flash its **existing named
partition** using the normal Retro-Go workflow:

```sh
python rg_tool.py --target esp32-p4-n16r32-st7796 --release-build build launcher
python rg_tool.py --target esp32-p4-n16r32-st7796 --port COM3 flash launcher
```

Use the correct target and port for your board. Do not flash a standalone
`launcher.bin` at address zero, and do not use ESP-IDF's placeholder partition
table to replace a multi-app installation. Full `.img` files use the regular
Retro-Go image installation workflow documented in `BUILDING.md`.

SD bundles contain theme artwork and the folder structure; the old
`retro-go/webui/index.html` is no longer distributed or served. Existing copies
can be removed. Device controls and diagnostics work without a mounted SD card;
file operations require one. Persisting settings still requires writable storage.

## Transfers and access

Use a trusted private network. This local HTTP manager has no user accounts or
transport encryption. Anyone who can reach it can manage the console. Disable
the manager on the device when it is not needed; do not expose it through a
router's port forwarding.

Mutations require `X-RetroGo: 1`; cross-origin browser preflights are refused.
File paths are confined to the storage root. Traversal, encoded NULs, malformed
URL escapes, hidden components, unsafe FAT names and reserved staging names are
rejected. Downloads use attachment responses with `nosniff`; filenames and
device data render as text. The page uses a restrictive content policy.

Uploads first write a reserved `.rg-upload` file. Failed or cancelled uploads
remove it and preserve the existing destination. Existing files return HTTP 409
until replacement is explicitly confirmed. A replacement additionally requires
`X-RetroGo-Overwrite: 1`; the old file moves to `.rg-backup` until publication
succeeds. Publication failure attempts to restore it. Keep power connected:
FAT renames are not a power-loss transaction. After power loss during replacement,
recover any `.rg-backup` directly from the card before retrying. Stale
`.rg-upload` files can be removed directly or replaced by retrying the same name.

Only one transfer runs at a time. The device serializes HTTP requests, so live
status pauses during uploads, and other file operations may wait or time out
until the active transfer finishes. Keep the tab open; queue history and file
objects are held only in browser memory. Folder filtering and sorting apply to
the current page of up to 128 entries; pagination reaches subsequent entries.

The Python SD backup utility sends the required headers, follows pagination,
and retains its existing overwrite/skip restore policy. Older third-party
clients need the same headers and must follow `next_offset` when `truncated`
is true. The legacy list/rename/delete/mkdir/touch commands and raw PUT/GET paths
remain available under those rules.

## Validation

```sh
python -m unittest discover -s tools/tests -v
python tools/test_webui.py --cc cc
python tools/test_sd_assets.py --cxx c++
python tools/test_sdcard_manager.py --cc cc
npm ci --prefix tools/webui-tests
npm exec --prefix tools/webui-tests -- playwright install chromium
npm test --prefix tools/webui-tests
```

The native harness executes the production path checks and transfer handlers,
including fragmented reads, abort cleanup, collision protection, replacement
rollback, disconnects, SD-independent flash serving, deferred controls, and the
physical-confirmation/failed-validation gates on firmware installation. Packaging tests verify
lossless and reproducible compression. Browser tests use an isolated in-memory
device API, run WCAG A/AA accessibility checks, and write desktop/mobile screenshots
to `build/webui/preview`.
Set `WEBUI_CHROMIUM` to use an existing Chromium executable. These checks do not
replace testing transfers and settings on the physical target.
