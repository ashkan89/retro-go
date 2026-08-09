# Retro-Go Media Player

A first-class media subsystem for Retro-Go on ESP32-S3, built as an IDF component
(`components/media`) and linked into the launcher. It adds a **Media** tab beside the
emulator tabs; selecting it hands the display over to a full-screen player and gets it back
cleanly when the user leaves.

---

## 1. Quick start

Copy music onto the SD card under `/sd/media`:

```
/media/
    Albums/
        Pink Floyd/
            The Dark Side of the Moon/
                01 - Speak to Me.flac
                02 - Breathe.flac
                02 - Breathe.lrc
                cover.jpg
    Music/
        Track01.mp3
    Podcasts/
        Episode01.mp3
    Playlists/
        Favorites.m3u8
```

The first time the Media tab is opened, the library is indexed in the background. The
browser stays usable throughout and shows live progress.

Player data lives in `/media/.retrogo-media/`:

| File | Contents | Rebuildable |
| --- | --- | --- |
| `library.idx` | One fixed-size record per track | yes (Rescan → Full rebuild) |
| `stats.bin` | Favourites, play counts, resume positions | **no** — user data |
| `queue.m3u8` | The queue, restored on the next run | yes |

Your media files are never modified.

---

## 2. Controls

Mappings are contextual and reuse Retro-Go's own input abstraction; no GPIO is referenced
anywhere in this component.

### Library / Queue

| Button | Action |
| --- | --- |
| Up / Down | Move the cursor |
| Left / Right | Page up / page down |
| A | Play the track, enter the folder or open the category |
| A (hold) | Context menu (Play next, Add to queue, Add to playlist, Favorite, …) |
| B | Up one level; at the top level, leave the player |
| START / SELECT | Next / previous player page |
| MENU | Player menu |
| OPTION | Quick settings overlay |
| Y (Queue page) | Remove the selected entry |

### Now Playing / Lyrics / Visualizer / Track Info

| Button | Action |
| --- | --- |
| A | Play / Pause |
| B | Back to the library |
| Left / Right (tap) | Previous / next track |
| Left / Right (hold) | Rewind / fast-forward, accelerating 5 → 10 → 20 → 30 → 60 s |
| Up / Down | Volume (on the Lyrics page with unsynced text: scroll) |
| START / SELECT | Next / previous page |
| MENU | Player menu |
| OPTION | Quick settings (Volume, Brightness, EQ, Shuffle, Repeat, Visualizer) |

### Equalizer screen

| Button | Action |
| --- | --- |
| Left / Right | Select band |
| Up / Down | Adjust gain (±12 dB) |
| A | Enable / disable the EQ |
| START | Preset list |
| SELECT | Reset to flat |
| B / MENU | Back |

Existing Retro-Go shortcuts are untouched: the launcher still uses SELECT/START for tabs and
MENU/OPTION for its own menus. The player only rebinds them while it owns the screen.

---

## 3. Supported formats

| Format | Decoder | Seek | Gapless | Notes |
| --- | --- | --- | --- | --- |
| MP3 | minimp3 (CC0) | yes | no | CBR, VBR (Xing/Info, VBRI) and free-format. Encoder delay is not compensated. |
| WAV | in-tree | yes | yes | PCM 8/16/24/32-bit and 32-bit float, 1–8 channels |
| FLAC | dr_flac (public domain) | yes | yes | 1–8 channels, any bit depth |
| AAC / M4A | — | — | — | Tags and artwork parse; no decoder is vendored (`MEDIA_CODEC_AAC`) |
| Ogg Vorbis / Opus | — | — | — | Tags parse; no decoder is vendored |

Anything else reports **Unsupported format** and, with *Skip failed tracks* on, playback
moves to the next entry.

Everything is normalised to interleaved signed 16-bit stereo at the track's native rate.
Mono is duplicated, channels beyond stereo are folded down, and the I2S clock is
reconfigured (muted, then faded back in) whenever the rate changes between tracks.

### Metadata

ID3v1, ID3v2.2/2.3/2.4 (including `TXXX` ReplayGain, `APIC` artwork, `USLT`/`SYLT` lyrics),
FLAC `STREAMINFO`/`VORBIS_COMMENT`/`PICTURE`, MP4 `ilst` atoms, and RIFF `LIST/INFO`.
Text is decoded from ISO-8859-1, UTF-16 (either endianness, with or without BOM) and UTF-8
into UTF-8 internally, so Persian, Arabic, Japanese and Chinese tags survive intact even
when the current font cannot render every glyph.

### Artwork

Priority: embedded picture → `<track>.jpg/.png` → `cover`/`folder`/`front`/`album` in the
containing folder → `<foldername>.jpg`. JPEG decoding uses TJpgDec **from the ESP32-S3
ROM**, so it costs no flash and descales by 1/2, 1/4 or 1/8 during decode — a 3000×3000
cover is never fully expanded in RAM. PNG reuses Retro-Go's lodepng path.

### Lyrics

`<track>.lrc` beside the file, then embedded `USLT`/`SYLT`/`LYRICS`. The parser accepts
multiple timestamps per line, out-of-order lines, `[ti:]`/`[ar:]`/`[al:]`/`[by:]`/`[offset:]`
tags (unknown tags are ignored) and plain unsynced text. Lookup during playback is a binary
search, not a scan.

---

## 4. Architecture

```
SD card
   |  fread (16 KB aligned)
   v
[media_io]        media_source.c   -> compressed ring (PSRAM)
   |
   v
[media_dec]       media_player.c + codecs/  -> PCM ring (PSRAM)
   |
   v
[media_audio]     media_audio.c    -> EQ -> gain -> limiter -> fade -> rg_audio_submit()
   |                                        |
   v                                        +-> media_fft_feed() (non-blocking tap)
speaker / headphones

[media_scan]  library indexing        (priority 1, yields per file)
[media_art]   artwork decode          (priority 1, backs off under pressure)
main task     UI render + input       (reads a snapshot, never the decoder)
```

| Task | Priority | Core | Purpose |
| --- | --- | --- | --- |
| `media_audio` | 6 | `RG_TASK_AFFINITY_AUDIO` | Drain PCM into I2S |
| `media_dec` | 5 | `RG_TASK_AFFINITY_AUDIO` | Decode + DSP block production |
| `media_io` | 4 | `RG_TASK_AFFINITY_IO` | SD prefetch |
| `media_scan` | 1 | `RG_TASK_AFFINITY_MAIN` | Library indexing |
| `media_art` | 1 | `RG_TASK_AFFINITY_MAIN` | JPEG/PNG decode |

Affinities come from the target's `config.h`; nothing is pinned by hand. Priorities sit
below `rg_audio`'s own I2S writer (9) and above the launcher's main loop (1).

**SD I/O, decoding and rendering never block one another.** The UI reads a
`media_snapshot_t` captured once per frame and posts commands; it never holds a decoder
lock.

### Playback position

Progress is derived from frames actually handed to the hardware
(`media_audio_position_ms()`), not from a wall clock, so the progress bar and the lyrics
cannot drift away from what is being heard.

### Resource manager

`media_player_pressure()` returns 0 (idle), 1 (playing comfortably) or 2 (a buffer is
running low). Both the library scanner and the artwork worker consult it before every unit
of work and back off at level 2. Audio continuity always wins.

---

## 5. Memory profiles

Selected at runtime from the detected PSRAM size (`media_profile.c`).

| | LOW (≤2 MB, N8R2) | NORMAL (2–6 MB) | HIGH (>6 MB, N16R8) |
| --- | --- | --- | --- |
| Compressed reserve | 48 KB | 128 KB | 384 KB |
| PCM ring | 6 K frames (24 KB) | 12 K frames | 24 K frames (96 KB) |
| Prebuffer | 2 K frames | 4 K frames | 8 K frames |
| Artwork cache | 192 KB / 4 entries | 512 KB / 8 | 1.5 MB / 16 |
| Cover size | 160 px | 200 px | 260 px |
| FFT | 128 pt / 16 bands | 256 / 20 | 512 / 24 |
| Target FPS | 30 | 30 | 60 |
| Blurred background | off | on | on |
| Crossfade allowed | no | yes | yes |
| Particle visualizer | no | no | yes |
| Resident index | 4 K tracks max | 8 K | 20 K |

PSRAM holds the ring buffers, artwork, the library index and decode scratch. Internal SRAM
holds only the small, latency-sensitive pieces: the audio output chunk and the TJpgDec work
pool. Every allocation that can fail uses `MEM_NOPANIC` and has a defined fallback.

Only a 24-byte entry plus a display-name pool is resident per track; full records are read
back from `library.idx` by record number through one persistent file handle. That is what
keeps a library far larger than PSRAM usable on an N8R2.

---

## 6. Settings

All under the `launcher` namespace with a `Media.` prefix, stored via
`rg_settings_*` (NVS-backed). Nothing large is ever written to NVS.

Background playback · Resume playback · Remember queue · Scan on startup · Normalization ·
Gapless · Crossfade · Skip failed tracks · Album-art background · Dynamic theme · Low
effects · Visualizer + FPS · Lyrics + offset · EQ enable/preset/7 band gains · Sleep timer ·
Debug overlay · Media root.

Play statistics are debounced (30 s) and written atomically; favourites are written
immediately because they are an explicit user action.

---

## 7. Audio focus and emulators

`media_audio_acquire()`/`release()` model ownership of the shared I2S device
(`NONE`/`PLAYER`/`EMULATOR`/`SYSTEM`). Two subsystems can never drive I2S at once.

`application_start()` in the launcher calls `media_suspend_for_app()` before
`rg_system_switch_app()`: the queue is saved, playback stops, and the hardware is handed
back with its original sample rate restored. Emulator audio always wins.

Background playback (default **Launcher only**) keeps music going while browsing the
launcher, and is dropped the moment an emulator is launched.

---

## 8. Build configuration

Feature flags live in `media_config.h` and can be overridden from the build system or a
target `config.h`:

```
MEDIA_PLAYER_ENABLE   MEDIA_CODEC_WAV   MEDIA_CODEC_MP3   MEDIA_CODEC_FLAC
MEDIA_CODEC_AAC       MEDIA_CODEC_OGG   MEDIA_CODEC_OPUS
MEDIA_EQ_ENABLE       MEDIA_FFT_ENABLE  MEDIA_LYRICS_ENABLE  MEDIA_ARTWORK_ENABLE
MEDIA_DEBUG_STATS
```

The launcher partition was raised from `0x180000` to `0x1C0000` on all six ESP32-S3 targets
to carry the decoders, DSP and UI (see each target's `env.py`).

---

## 9. Known limitations

* **AAC/M4A, Ogg Vorbis and Opus have no decoder.** Their tags, duration and artwork parse
  correctly and the codec registry already knows about them, so adding a `codec_*.c` is the
  only work required. Files in these formats currently report *Unsupported format*.
* **Gapless** is exact for FLAC and WAV. MP3 does not compensate encoder/decoder delay, so a
  few milliseconds of silence remain between MP3 tracks.
* **Crossfade** is exposed as a setting and disabled where the profile cannot afford a second
  decoder; the mixing path itself is not implemented, so the option currently behaves as
  "off" at playback time.
* **Waveform overview** (`waveform_overview` in the profile) is reserved but not generated;
  the seek bar is linear.
* **Search** is not implemented — the input hardware has no usable text entry, so the browser
  offers category, folder, album, artist and genre navigation instead.
* **Bookmarks** for long recordings are not implemented; the resume-position mechanism
  (automatic for files longer than 15 minutes) covers the common podcast/audiobook case.
* **RTL shaping** is not performed. UTF-8 metadata is preserved byte-exact and truncation is
  always codepoint-aligned, and all text layout goes through `media_ui_draw_marquee`, so
  bidi/shaping can be added in one place later. Glyphs the current font lacks render as the
  font's replacement rather than corrupting the string.
