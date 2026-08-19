# Retro-Go 3.7.0 (unreleased)

## Launcher - game lists, scanning and boot

- Launcher: Rom lists are cached on the card (`/retro-go/cache/roms_<system>.list`) and read back
  instead of walking the folders again. A tab opens instantly, and the game count is shown on the
  carousel before a tab has ever been opened - no more tabs stuck on "Loading..." until you enter
  them. The cache records the folders it came from with their timestamps, so a folder that changed
  is rescanned on its own
- Launcher: New **Scan Game List** action in the menu rescans every rom folder and rewrites the
  caches. This is what to use after copying new games onto the card; it does not trust the cached
  timestamps (FAT does not always update a directory's when a file is added)
- Launcher: New **Reboot** action in the menu, which restarts the console and shows the boot
  animation on the way back up (a software restart is not a cold boot, so the animation is
  requested explicitly rather than being skipped)
- Launcher: The boot animation is about twice as long, with each beat given room to read: the scene
  settles, the mark drops and switches on, the wordmark comes up, the highlight crosses it
- Launcher: *Scroll mode* is now *List scrolling* and applies only to the game list. Menus and
  dialogs scroll one row at a time instead of paging, so stepping past the last visible row moves
  the list by one line and keeps your place

## Firmware update fixes

- All: Every failure while writing flash now says what went wrong. `write_flash_range()` had five
  silent `return false` paths (out of memory, unaligned range, erase failed, image read failed, flash
  write failed): the install aborted, the caller had nothing to report, and the user was left with a
  screen that said only that the update had failed. Each one now returns a reason that is shown and
  logged
- Launcher: Downloading an update now deletes the firmware images already in `/retro-go/firmware`
  first. Nothing ever cleaned them up, so every update left another few megabytes behind - which
  wastes space a 4 MB image may need (a truncated download then fails its checksum), and leaves stale
  images where the factory app looks for the one to apply
- Factory: The pending image is chosen newest-first instead of taking whatever the directory listed
  first, so an old image cannot win over the one just downloaded. It also shows which file it is
  checking, because verifying an image means a full CRC pass plus comparing every partition against
  flash - seconds per file, and previously silent
- All: Clearing the panel (`rg_display_clear`) now drops the GUI's backdrop. A dialog composited over
  a surface that no longer matches the screen would show the screen that used to be there, which is
  exactly what happens during an update, where each stage clears the screen first
- Launcher: A device whose firmware has no factory partition cannot flash an image by itself. Instead
  of "Firmware updater app not found!" it now says so, and says what to do: the downloaded file is
  kept in `/retro-go/firmware`, flash it over USB once and updates work from the device afterwards

## Caches and the updater

- All: **Clear cache** (debug menu) and **Reset settings** now clear every cache on the card, not
  just the shared cache folder: rom lists, CRC checksums, the saved clock, per-emulator cache files,
  and the media player's library index. It goes through one new entry point, `rg_system_clear_cache()`,
  which also raises `RG_EVENT_CLEAR_CACHE` so an app can drop caches the core knows nothing about
- All: What is *not* cleared is user data, deliberately: saves, save states, screenshots, cover art,
  themes, borders, playlists, and the media player's favourites, play counts, resume positions and
  saved network locations (that is why the player keeps `stats.bin` separate from `library.idx`)
- Launcher: Clearing the cache also drops the rom lists held in RAM, so nothing stale is written
  back over the files that were just deleted
- Launcher: A firmware download can be cancelled. The download loop polls input on every chunk, so
  buttons respond while it runs (and a keypress wakes the screen again once it has dimmed or switched
  off - before, nothing was reading the gamepad, which is what wakes it). **B** asks whether to stop;
  confirming deletes the partial file and returns to the menu
- Launcher: The download card is composited through the new overlay API instead of keeping a
  full-screen scratch surface of its own, which saves ~150 KB and removes a case where it could be
  mistaken for the screen's backdrop

## Interface - flicker and fonts

- All: Dialogs, messages, the on-screen keyboard and its input field are composited into an
  offscreen buffer and sent to the panel in a single transfer. Before, every card was painted
  straight to the LCD piece by piece on every keypress, which is what made menus flicker while
  scrolling. Where the app owns a surface (the launcher, the media player), the overlay is
  composited over that surface, so a dialog now sits over the real background with its translucency
  and shadow intact; where it does not (a game), the card sits on a flat plate instead
- All: `rg_gui` drawing now goes through a target with its own origin, stride and clip rectangle,
  which is what lets the same drawing code paint the screen, an app surface, or a small window of
  the screen
- All: New **Sans 12** font, and the existing Sans font is now named **Sans 15**. Both carry the
  Arabic block and the Arabic presentation forms
- All: Persian and Arabic text now renders properly: `rg_text_shape()` joins each letter into its
  contextual form (isolated/initial/medial/final), builds the required lam-alef ligatures, and lays
  the line out right to left while numbers and Latin words inside it keep reading left to right.
  Text with no right-to-left characters is returned untouched, so it costs one string scan

## Interface

- All: New drawing primitives in `rg_gui`: rounded cards, translucent fills, soft drop shadows,
  gradients, lines, discs, scrollbars and progress bars. Translucency composites against the
  surface when there is one to read back (launcher, media player, boot screen) and against the
  theme background when there is not (dialogs over a running game), so one set of code draws the
  same design in both places
- All: Dialogs redrawn as rounded cards with a drop shadow, a header chip with an accent bar, a
  selection pill instead of an inverted row, values right-aligned in a dimmer color, real
  separators instead of a row of dashes, and a scrollbar in the card's gutter in place of the
  little arrows
- All: Status icons redrawn. The battery is a rounded cell with a contact tip, a charge bar that
  keeps its green/amber/red meaning, a lightning bolt while charging and a blink when nearly
  empty; Wi-Fi is a three-bar meter driven by the actual RSSI, sweeping while it associates. Over
  a theme background image the cluster sits on a translucent chip so it stays readable
- All: The on-screen keyboard (Wi-Fi passwords, renames) now has rounded keys, an accent-filled
  selected key and a dark input field with an accent outline
- All: Save-slot previews get a rounded label chip; the frame still shows at a glance whether the
  slot holds a state
- Launcher: The browser is laid out in columns - a slim header band with the system logo and name,
  the list on the left with a scrollbar, and the cover/save preview in a card of its own on the
  right. Covers no longer float on top of the game names
- Launcher: Selected row drawn as a pill with a leading accent bar, and a name too long to fit
  scrolls (one character at a time, after a pause) while it is selected
- Launcher: Hint bar along the bottom showing what the buttons do, with the item counter on the
  right
- Launcher: The system carousel shows the logo and banner on a translucent card, the systems as
  pills at the bottom, and the battery/clock cluster it was missing
- Launcher: Backgrounds get a scrim behind the header and footer so text stays readable over any
  theme artwork, and the flat no-image background gets a soft accent gradient
- Launcher: Animated boot screen on cold boot - a synthwave horizon with a scrolling grid, a
  striped sun, twinkling stars, the console mark dropping in and switching on, and the wordmark
  with a specular sweep. It is drawn from shapes rather than a bitmap, so it costs no flash and
  fits every panel size. Any button skips it, and it can be turned off in
  *Options > Launcher options > Boot animation*
- Launcher: Update download progress uses the same card and bar as the rest of the UI
- Theming: `theme.json`'s `dialog` section gains `accent`, `accent_dim`, `highlight`, `surface`,
  `surface_alt`, `divider`, `text_dim` and `item_value`. All are optional and derived from the
  existing keys when absent, so older themes keep working; colors may now be written as RGB888
  (`0x5AAAFF`) as well as RGB565
- Theming: All 13 bundled themes rewritten with a full palette each, one hue per theme - Art Book
  Next (indigo), Commic Book (magenta), GBZ (lime), Super Lopez (orange), base (monochrome), basic
  (cyan), box (amber), classic (navy), clean (mint), default (azure), noir (crimson), pixel
  (violet) and tv (yellow on midnight blue). Each also gets its own `media` section and four
  distinct launcher variants (white-on-accent, second colour, solid accent pill, high contrast),
  and every theme's `description` is now its folder name

# Retro-Go 3.6.0 (2026-08-09)

## Media Player (new)

- Launcher: New **Media** tab opens a full-screen music player for files under `/sd/media`
- Media: MP3 (minimp3), WAV and FLAC (dr_flac) playback behind a common codec interface
- Media: Decoding, SD reading, audio output and rendering run on separate tasks, so none of
  them can block another; progress is derived from frames actually played rather than a timer
- Media: Adaptive buffering with a compressed prefetch ring and a decoded PCM ring, sized at
  runtime from the detected PSRAM (N8R2 and N16R8 get different budgets)
- Media: Library index on the SD card with incremental rescan, atomic writes and a version
  check; favourites, play counts and resume positions are kept in a separate file so a full
  rebuild never loses them
- Media: Browse by Folders, Albums, Artists, Genres, Playlists, Favorites, Recently Played,
  All Tracks and Network
- Media: Now Playing, Lyrics, Visualizer, Queue and Track Info pages
- Media: Album art from embedded tags (ID3 APIC, FLAC PICTURE, MP4 covr) or `cover`/`folder`/
  `front` images beside the track. JPEG is decoded by TJpgDec out of the ESP32-S3 ROM, so it
  costs no flash and never expands a large cover in RAM
- Media: Accent colours are extracted from the artwork, with a blurred darkened background
- Media: Synchronised lyrics from a `.lrc` beside the track or embedded tags, with a
  per-track timing offset
- Media: 7-band biquad equaliser with 10 presets, automatic headroom and a soft limiter
- Media: 12 visualiser modes driven by a real FFT of the audio being played
- Media: Queue with a shuffle bag (no immediate repeats), four repeat modes, and `.m3u`/
  `.m3u8` playlist support
- Media: Sleep timer, background playback while browsing the launcher, gapless playback for
  FLAC and WAV, and ReplayGain normalisation
- Media: Tags read from ID3v1, ID3v2.2/2.3/2.4, FLAC Vorbis comments, MP4 atoms and RIFF
  INFO, decoded to UTF-8 so non-Latin metadata survives intact
- Media: Playback stops and releases the audio hardware before an emulator is launched

## Media Player - network

- Media: Play internet radio and any `http(s)` audio URL
- Media: Browse a shared folder served over HTTP or WebDAV (nginx/Apache autoindex,
  `python3 -m http.server`, Nextcloud, most NAS boxes) and play from it like the card
- Media: Icecast/Shoutcast `StreamTitle` updates drive the Now Playing text
- Media: Station URLs that point at a `.m3u`/`.pls` are resolved automatically, trying each
  mirror in turn; HLS manifests are reported rather than half-played
- Media: Stream format is identified from the server's `Content-Type` first, so an
  extension-less or query-suffixed URL is recognised correctly
- Media: A dropped stream reconnects, resuming mid-file when the server supports ranges
- Media: Saved locations live in `/media/.retrogo-media/network.txt`, editable on a PC

## Media Player - theming

- Media: The tab is themeable as `mediaplayer`, with `logo_mediaplayer.png`,
  `banner_mediaplayer.png` and `background_mediaplayer.png`; all three ship built in
- Media: `theme.json` gains a `media` section for the player's own colours. With Dynamic
  theme on, the accents still follow the album art and the rest of the section applies

## Fixes and core changes

- All: The screen-timeout tick now runs only on the UI task. It hangs off `rg_task_delay()`,
  so any task could end up redrawing the whole UI on its own stack - which overflowed
  `rg_sysmon` when several background tasks were running
- All: `rg_task_t` table grown from 8 to 16 slots; the previous size left one free slot with
  the media player running
- All: Fixed the WS2812 status LED never completing a transfer. `rmt_tx_wait_all_done()`
  takes milliseconds but was being passed `pdMS_TO_TICKS()`, and a single timeout leaves the
  RMT channel permanently wedged, so it is now rebuilt rather than re-enabled
- All: LED updates are rate limited; the SD activity indicator was driving a blocking RMT
  transfer around every block transfer
- All: `rg_network_http_open()` accepts request headers and reports response headers, and
  follows 303/307/308 redirects as well as 301/302
- All: The legacy I2S deprecation notice is suppressed on ESP32-S3 targets
- All: ESP32-S3 launcher partition grown from 1.5 MB to 1.75 MB to hold the media player
- Media: Remote files now show their real title, artist, album and cover art. Tags are read
  with an HTTP range request on the artwork worker, so playback still starts immediately
- Media: Fixed remote tracks never actually getting those tags or their cover. The artwork
  worker defers while the audio buffer is low, and that test measured the compressed reserve as
  a percentage of its ring - but the network ring is deliberately sized for a server's opening
  burst, so a healthy stream sits at a low percentage for as long as it plays. The reserve is
  now judged in seconds of audio, and the deferral is bounded
- Media: Fixed a crash when leaving the player with music still going. The artwork cache was
  reclaimed without its lock while the worker was still running, freeing surfaces it was about
  to publish
- Media: A failed remote tag read is retried up to three times instead of being cached as
  "no artwork" for the rest of the track
- Media: Station name and genre now appear as soon as a stream's headers arrive, rather than
  waiting for the first inline title - which some stations never send
- Media: Cover art for radio, where the station names one in its metadata (`StreamArtwork`, or
  `StreamUrl` when it points at an image)
- Media: A coverless broadcast no longer queues artwork lookups against its own stream URL
- Media: The Icecast metadata block moved off the IO task's 8 KB stack onto the heap, and is
  only allocated for a station that sends metadata
- Launcher: Regenerated `images.c`, which had gone out of sync with
  `themes/default/background_msx.png`

## Media Player - delayed live playback

- Media: New **Live stream buffer** setting: `Live` (default), `12 s`, `15 s` or `20 s`. With a
  delay set, output is held silent until that much of the broadcast is banked, so playback runs
  that far behind the live edge and a network stall shorter than the delay is inaudible
- Media: The compressed ring is sized from the setting rather than the profile alone, with a
  per-profile ceiling; a stream above 192 kbps banks less than the full delay rather than
  failing, and the trim is logged
- Media: A pre-roll that stops making progress for 8 s starts playing with what it has, so a
  server that goes quiet mid-fill cannot leave the player waiting for ever
- Media: Buffering progress is shown as `Buffering 8s/20s` rather than a bare spinner

Not yet supported: AAC/HE-AAC, Ogg Vorbis and Opus decoding (the formats are identified and
named, but no decoder is bundled), HLS, and SMB/CIFS shares.


# Retro-Go 1.46 (2025-11-07)
- Launcher: Added virtual keyboard to manage wifi networks
- Launcher: Diacritics are now correctly shown in filenames
- All: Overclock works better (original esp32 only)
- All: German translations has been added
- All: On-screen low battery indicator


# Retro-Go 1.45 (2025-07-27)
- SMS/GG: Fixe broken inputs in many games since 1.40 (#198)
- SNES: Increased max supported ROM size on some devices
- Launcher: Fixed network status in options menu
- All: Improved D-PAD input accuracy on some devices
- All: French translation now has accents
- New device support: LILYGO T-Deck Plus
- New device support: Null Nano by Ampersand
- New device support: crokpocket
- New device support: VMU


# Retro-Go 1.44 (2025-02-03)
- SNES: Larger ROMs are now supported (on devices with enough memory)
- All: User interface is now multi-language (currently english and french)!
- New device support: Retro-Ruler
- New device support: Byteboi rev.1
- New device support: Rachel ESP32


# Retro-Go 1.43 (2024-09-02)
- All: Added ZIP support (large 4MB+ GBC roms not supported unfortunately)
- Launcher: Added menu option to pre-compute all CRC32s (for cover art)
- Launcher: Officially support name-based cover art (eg: `/covers/nes/game name.png`)
- Launcher: Improved responsiveness when cover art/save preview is enabled
- Launcher: Added network status/details in wifi dialog


# Retro-Go 1.42 (2024-06-05)
- PCE: Fixed out-of-bounds VCE regs access (Crash in Raiden, probably others)
- NES: Fixed panic in some games (Snow Brothers, Chip'n'Dale, probably others)
- Launcher: Fixed stability issues when cycling through all tabs


# Retro-Go 1.41 (2024-04-16)
- MSX: Added fMSX emulator
- COL: Added virtual keyboard so that more games work
- COL: Fixed savestates not working
- All: Improved scaling; you can now zoom to any size you want!
- All: Eliminated battery icon flicker
- All: Added overclock option in the debug menu (need feedback before making it permanent)


# Retro-Go 1.40 (2024-02-27)
- NES: Added sound emulation to mapper 19 (Pacman Championship Edition)
- NES: Added 2KB CHR support to mapper 64 (Rolling Thunder)
- NES: Fixed Battletoads (now works past level 2 but there's still occasional jitter)
- NES: Fixed Battletoads Double Dragon (freezing)
- NES: Fixed Teenage Mutant Ninja Turtles 3 (HUD jumping around in level 2)
- NES: Fixed other minor glitching in mapper 4 (MMC3) games
- NES: Fixed text garbled in Gun-Nac
- COL: Added palette selector (also for SG-1000)
- COL: Added support for SGM
- All: Added support for custom scaling dimensions
- All: Added border support (to replace black bars when not using full screen scaling)
- All: Fixed savestate wouldn't be loaded on next boot after saving
- All: Fixed partial screen updating sometimes caused artifacts
- Launcher: Long file names are now working correctly (though still not recommended)


# Retro-Go 1.39 (2023-??-??)
- Launcher: Reject names that are too long instead of being weird
- Theming support is now tested and documented
- Added DAC pinout for ODROID-GO


# Retro-Go 1.38.1 (2023-04-02)
- GBC: Added switch to disable RTC sync with system time
- Launcher: Fixed crash when files without extensions were present


# Retro-Go 1.38 (2023-03-28)
- GBC: Added support for MBC30 (For Pokemon Crystal romhacks)
- Launcher: Added a new scroll behavior (Options -> Scroll mode)


# Retro-Go 1.37 (2022-12-30)
- SNES: Fixed controls menu labels
- GEN: Small performance improvement
- Launcher: Added tool to download updates


# Retro-Go 1.36.3 (2022-12-14)
- SNES: Fixed freeze in controls menu (hopefully for real this time...)


# Retro-Go 1.36.2 (2022-12-07)
- SNES: Fixed crash in controls menu
- Launcher: Start wifi access point


# Retro-Go 1.36.1 (2022-11-29)
- Fixed MRGC/esplay builds


# Retro-Go 1.36 (2022-11-28)
- GBC: Fixed Pokemon Trading Card Game
- SNES: Added support for ROMs with extra headers
- SNES: Added low pass filter toggle
- GEN: Added support for interleaved ROMs
- Lynx: Changed samplerate to fix some audio issues
- Launcher: Fixed recently played list
- Wifi and WebUI improvements
- Improved recovery mode reliability


# Retro-Go 1.35 (2022-10-18)
- SNES: New Snes9x port (based on 2005 version) with sound!
- GEN: Improved compatibility (updated from bzhxx's upstream)
- GW: Added Game & Watch emulator by bzhxx
- Lynx: Fixed crash when opening options menu
- Launcher: Added web file manager
- Launcher: Added network time sync
- Launcher: Increased max filename length to ~72 characters
- All: Fixed some crash recovery issues
- Releases are now built with esp-idf 4.3


# Retro-Go 1.34 (2022-09-18)
- SNES: Fixed crash when opening menu
- NES: Implemented mapper 185
- PCE: Fixed crash when quitting emulator
- PCE: Improved stability and compatibility
- GBC: Fixed Resident Evil Gaiden not showing test (thanks to ddrsoul)
- All: Improved SD Card compatibility


# Retro-Go 1.33 (2022-08-01)
- Added Genesis/Megadrive emulator
- Added multiple save state support (per game)
- DOOM: Added IWAD selector when launching a mod (PWAD)
- Launcher: Fixed buttons not responding during `CRC32...`


# Retro-Go 1.32.1 (2022-05-21)
- Launcher: Fixed savestate screenshot using the wrong file name (#13)


# Retro-Go 1.32 (2022-04-27)
- Launcher: Added subfolder navigation (there are still some sorting issues)
- Launcher: Improved responsiveness when covers are enabled but not installed
- All: Speedup mode improvements: More granularity, added 0.5x mode, working audio, DOOM is now supported
- All: Moved saves on retro-go to be in line with other devices (a notice will be displayed on first boot)
- All: Added .img generator, to use with esptool.py
- All: Pressing start+select will open options menu (on devices with no option button)


# Retro-Go 1.31.1 (2022-03-08)
- All: Fixed sound on MRGC-G32


# Retro-Go 1.31 (2022-02-15)
- Launcher: Fixed occasional crash when changing tab
- GBC: Fixed high cpu usage / slowdowns
- GBC: Fixed RTC format in SRAM
- All: Fixed crash when changing audio sink in game
- All: Fixed occasional settings reset if a panic occurred while a key held down
- All: Improved panic handling, more detailed crash.log


# Retro-Go 1.30 (2022-02-02)
- NES: Fixed Lagrange Point
- NES: Added support for a few untested mappers
- NES: State files are now smaller (faster load/save)
- PCE: Many sprites issues fixed by Macs75
- PCE: New save state format (incompatible with old saves, but extensible going forward...)
- Added key repetition in dialogs (when holding an arrow key, for example)
- We now have continuous integration thanks to tomzx


# Retro-Go 1.29.1 (2021-12-23)
- Fixed settings not remembered per emulator
- Fixed recent/favorites should be cleared on settings reset
- Added back "Reboot to firmware" in about dialog
- Fixed other minor bugs


# Retro-Go 1.29 (2021-12-20)
- Launcher: New look and feel
- NES: Show when NSF is playing
- DOOM: Added `Toggle Map` in the DOOM menu
- DOOM: Fixed issues affecting the MRGC-G32 (sound and image)
- Replaced LuPng/zlib with lodepng


# Retro-Go 1.28 (2021-11-23)
- GBC: Dinky Kong & Dixie Kong (and probably others)
- GBC: Fixed SRAM saving not working correctly (thanks to Jarrad Edwards!)
- NES: Added basic NSF support
- NES: Added support for mapper 30
- SMS: Performance improvement
- DOOM: New PrBoom port included. (DOOM I, II, SIGIL, and Freedoom work well but occasional OOM crashes remain)
- Launcher: Got rid of the occasional SPI mutex error
- MRGC G32 (GBC) is now fully supported (Hopefully)


# Retro-Go 1.27.2 (2021-10-13)
- SMS: Enabled SG-1000 support
- GBC: Hide palette option when running GBC game
- Launcher: Adapt cover art size to screen resolution
- Fixed multiple display corruption issues with dialogs


# Retro-Go 1.27 (2021-09-27)
- NES: Implemented mappers 76 and 206
- NES: Optimized mappers 9 and 10
- GBC: Fixed Dinky Kong & Dixie Kong
- Minor menu reorganization
- First alpha build for MRGC-G32 (GBC) devices


# Retro-Go 1.26.3 (2021-08-29)
- Fixed typo resulting in corrupt PNG decoding


# Retro-Go 1.26.2 (2021-08-25)
- PCE: Performance improvement
- GBC: Performance improvement
- GBC: Fixed score not shown in Donkey Kong
- Launcher: Improved PNG support
- All: Replaced miniz by zlib which should resolves all save-related crashes


# Retro-Go 1.26.1 (2021-07-31)
- Launcher: Fixed subdirectory display (again)


# Retro-Go 1.26 (2021-07-29)
- GBC: Experimental BIOS support (put your files in /bios/gb_bios.bin /bios/gbc_bios.bin)
- NES: Added Mesen roms database to patch wrong headers
- All: Option to disable partial screen updating (it seems to cause glitches for some people)
- Launcher: Option to enable/disable any tab
- Launcher: Fixed subdirectory display
- Launcher: Fixed sorting bugs
- Launcher: Recently played list
- Dev: Now able to build on esp-idf 4.1 and 4.2


# Retro-Go 1.25.3  (2021-05-30)
This is a bug fix release that fixes the following issues:
- Occasional crash when saving a game
- Corrupted or missing save screenshot
- Menu dialog sometimes cut off with big fonts (now can scroll)
- Games in subfolders now shown in the launcher (still no sub navigation but everything will be listed at least)


# Retro-Go 1.25.1  (2021-04-28)
- Fixed: dialog multiline text mangling one character
- Fixed: font selection not wrapping around correctly
- Fixed: occasional crash when saving state (it might still crash but the save will at least finish first)


# Retro-Go 1.25    (2021-04-11)
- All: Proportional fonts support with correct alignment
- All: Crash log is now saved to the SD Card if possible. Please share it if you encounter a non-obvious crash.
- All: The settings format has changed to allow more flexibility (some of your settings will be lost, sorry!)
- GG/COL/SMS: Now have independent settings, you can set different filters and scaling per console!
- Launcher: Idle CRC32 caching, just let your GO sit for a while to never see "CRC32..." again!
- Launcher: Improved responsiveness


# Retro-Go 1.24    (2021-02-26)
- NES: Implemented Mapper 19 IRQ counter (fixes Pacman Championship Edition (still sound issues))
- NES: Significant performance improvement in some games
- NEX: Fixed Ninja Gaiden 3
- PCE: Fixed a rare crash when saving state on large games
- PCE: Added option to force 8bit audio
- GBC: Emulation is now running closer to real speed (60fps instead of 59, real is 59.7)
- GBC: Fixed Kirby Dreamland 2
- GBC: Fixed Fatass
- GBC: Fixed the harsh flicker when starting some games
- GBC: Hopefully improved how SRAM is handled
- SNES: Save states now working
- SNES: Fixed wrong colours in some games
- SNES: Choice of controls style
- SNES: 24Mbit ROMs support
- SNES: Performance improvements (still very slow)
- ALL: Fixed dialog sometimes crashing when pressing the wrong button
- ALL: Show emulation speed in status bar instead of just FPS
- ALL: Added soft reset option
- ALL: Enabled exFAT support
- ALL: Debug menu is now easier to access (currently pretty useless): press start when main menu is open
- ALL: Option to use LED to show disk activity (also fairly useless)

Note: You should backup your GBC saves before trying this build.

(See [1.19](https://github.com/ashkan89/retro-go/releases/tag/1.19) to download cover art.)


# Retro-Go 1.23    (2021-01-19)
- GBC: Fixed palette and rtc not always reset correctly
- GBC: Fixed uninitialized values when loading/saving states (fixes minor glitches)
- NES: Compatibility improvement (fixes Dragon Warrior IV)
- SMS and GBC and NES: Small performance improvements and misc. bug fixes
- SNES: First preview! (Please don't report bugs, it is not playable yet)


# Retro-Go 1.22    (2020-12-31)
- GBC: Significant performance improvement (greatly improves Mario Tennis, Super Mario Land DX, Radikal Bikers, and more)
- GB:   Fixed Link's Awakening glitch
- PCE: Small performance improvement


# Retro-Go 1.21.1  (2020-12-26)
- PCE: Improved emulation accuracy and performance
- PCE: DDA emulation now mostly works (sound effects, voices)
- PCE: Option to crop overscan
- NES: Small performance improvement
- Launcher: A screenshot of your saved game will be shown if no cover found (configurable, not available in PCE yet)
- Tuned the auto frame skipping to reduce stutter in Pokemon


# Retro-Go 1.20.2  (2020-11-02)
- PCE: Fixed missing enemy sprites in Bonk
- PCE: Audio should be better in games like Bonk
- GBC: SRAM always generated when saving state and file now compatible with VisualBoyAdvance


# Retro-Go 1.20.1  (2020-11-01)
- PCE: Workaround for glitch in Splatterhouse (CRC D00CA74F)
- PCE: Fixed cropping offset for high res games
- PCE: Greatly improved performance for all games


# Retro-Go 1.19    (2020-10-01)
- LYNX: Rotation support
- LYNX: Added cover art
- ALL: New "medium" font size
- ALL: Slightly faster boot

Also some internal changes:
- SMS: Refactored the z80 emulation for more performance and smaller code size
- GBC: Misc cleanup for smaller code size
- ALL: Python build scripts, mkfw and gcc are no longer necessary
- ALL: Implemented a basic profiler based on GCC function instrumentation


# Retro-Go 1.18    (2020-08-24)
- RETRO: Favourites support (let me know if it can be improved)
- RETRO: Fixed misc navigation bugs
- NES: New MMC1 implementation to improve compatibility (eg: Dragon Warrior 4 now boots but crashes for other reason)
- NES: Battletoads "fixed" (workaround. still glitches, but it seems playable)
- GBC: Fixed garbled tiles in some games (eg: Speedy Gonzalez)
- PCE: Fixed horizontal lines in some games
- SMS/GG: Improved performance, 25% less frameskip
- ALL: Fixed stutter when changing a setting (on slow SD Card)
- ALL: Better panic handling that might show the crash reason now


# 2020-08-01
- NES: Compatibility hack to fix glitches in Zen Intergalactic, Power Blade, Kirby, SMB3
- NES: Fixed volume issues above level 5
- Launcher: Misc UI improvements
- Settings are now stored on the SD card
- Retro-Go is now compiled with esp-idf 4.0


# 2020-06-29
- Added file property dialog (press B)
- Minor UI tweaks
- Fixed crash on some devices when the battery is full or charging
- Fixed stuttering in GB/GBC emulation
- Fixed most scaling issues (some games still have horizontal bars in PCE)


# 2020-06-13
- NES: Fixed palette not applied
- NES: Fixed some bugs with auto crop
- PCE: Fixed US encoded roms
- PCE: Fixed Legend of Hero Tonma
- PCE: Fixed scaling bug causing black screen in many games (eg Salamander)
- PCE: Implemented Street Fighter 2 mapper
- Lynx: Lynx is now playable (rotation support still missing)


# 2020-05-30
- NES: New option to detect and crop the blank leftmost column in some games (eg SMB3)
- NES: Implemented Mapper 193 (War in the Gulf)
- NES: Fixed shadows in Aussie Rules Footy
- NES: Fixed games relying on 32K banks
- NES: Fixed more Chinese bootlegs
- NES: Improved sound on PAL games (speed is still a bit off in some games)
- NES: New save/load code that will soon be compatible with saves from popular emulators
- GB:  Fixed Donkey Kong statistics not showing (Game-specific hack)
- GBC: Fixed Fushigi no Dungeon - Fuurai no Shiren GB2 text not showing (Game-specific hack)
- PCE: Cover art improved
- All: Saves are now atomic, an interruption while saving (crash/battery/sd error/etc) will no longer corrupt your save file
- All: Battery and FPS indicators when you open in-game menu
- All: Option to select startup app (always launcher vs last used game)
- All: Firmware size is back to a mere 2.56MB (including PCE and Lynx!)


# 2020-05-04
- PCE: Improved performance, all tested games run at 100% speed with higher refresh rate
- PCE: Save states fully implemented
- NES: Enabled support for mappers 41, 42, 46, 50, 73, 93, 160, 229 (bootlegs/multicarts)
- NES: Implemented mappers 162 and 163 (A dozen of Nanjing games)
- Smaller firmware size (3.5 to 3MB)


# 2020-04-27
- PC Engine emulation!
- Colecovision no longer needs a BIOS
- Smaller binary sizes (though the .fw is padded to the same as before)
- License is now GPL2 for all components in the project
- NES: Option to disable overscan
- GB: DMG GREEN palette by 8xpdh
- GB: GBC's game-specific colorization support
- GB: Option to save/load SRAM automatically (careful: causes stuttering in many games)

Known issues:
- Save states support in PCE is experimental. Loading is also manual only (start a game -> Reload).


# 2020-03-20
- NES: PAL games support
- NES: Fixed Cheetahmen 2 (Impl. Mapper 228)
- GB: Fixed Altered Space 3-D Adventure
- GB: Fixed Faceball 2000
- GB: Performance improvement


# 2020-03-13
- NES: Color palette support (FirebrandX)
- NES: Option to disable sprite limit (sprite flicker)
- NES: Fixed SMB3 (Broken in 2020-03-10)
- NES: Fixed The Simpsons (Broken in 2020-03-10)
- NES: Fixed Owlia (Broken in 2020-02-29-exp)
- NES: Fixed Castlevania III (It's playable but there are issues) (Never worked before)
- NES: Fixed Choplifter (Never worked before)
- All: Fixed filter alignment issue (mostly visible in GB)


# 2020-03-10
- New: Bilinear filtering available for all emulators
- Launcher: Fix SPI conflicts that froze the application
- NES: Fix crash during save/load in some games
- GBC: Fixed Worms Armageddon
- GBC: Fixed Metal Gear Solid
- NES: Fixed Teenage Mutant Ninja Turtle III
- NES: Fixed Felix the Cat
- NES: Fixed Tetris 2 + Bombliss
- Misc bug fixes


# 2020-02-23
- Display code optimization with 5-10% FPS throughput increase
- Friendlier in-game fatal errors that allow to return to launcher
- Many bug fixes


# 2020-02-19
- GB/GBC: More of the ROM is preloaded to fix stuttering in some games
- GB/GBC: Now using partial screen updates, allowing 60fps in many games
- Turbo speed support in all emulators
- Removed interlacing. The replacement method looks better (or at least equal) and works for all three systems
- Launcher: File extensions are now hidden


# 2020-02-11
- New scaling options
- 8MB GBC game support
- Significant size reduction
- Bug fixes


# 2020-02-05
- Fixed Game Gear aspect ratio
- PNG support for cover art
- Option to reduce cover art loading delay
- Experimental ZIP support for NES/SMS/GG ROMS


# 2020-02-01
- Initial release
