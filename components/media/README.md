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

Sources are interchangeable: a queue entry, a playlist line or a "play this" action can be
a path on the card or an `http(s)://` URL, and every decoder below works over either.

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

## 4. Network media

### Playing a URL

**Network → Add a network location...** takes an `http(s)://` address and offers to browse it
as a folder, save it as a radio station, or play it immediately. Saved entries live in a
plain tab-separated file at `/media/.retrogo-media/network.txt`:

```
# type<TAB>name<TAB>url   (type: dir, radio or url)
dir	NAS Music	http://192.168.1.10/music/
radio	BBC Radio 1	http://stream.example/radio1
```

Editing that file on a PC is far less painful than typing URLs on an on-screen keyboard, and
`.m3u`/`.m3u8` playlists dropped into `/media` may contain URLs too — they are queued exactly
like local tracks.

### Browsing a shared folder

`Network → <a saved folder>` lists a remote directory and lets you walk into it, queue it and
play from it just like `/media`. Two listing dialects are understood:

* **WebDAV** — Nextcloud, Synology/QNAP, `rclone serve webdav`, Windows "Web Sharing".
* **HTML directory index** — nginx `autoindex`, Apache `Options +Indexes`,
  `python3 -m http.server`.

Only links that stay under the folder being listed are followed, so an index page that links
off-site cannot walk the scanner around the server.

**There is no SMB/CIFS client.** ESP-IDF does not ship one, and vendoring an SMB stack is far
more surface area than a listing needs. If your files are on a Windows share, the practical
route is to expose the same folder over HTTP or WebDAV.

Tags and cover art work on remote files too. Reading them costs an HTTP range request for
the head of the file, so it is done by the artwork worker rather than at open time: the track
starts playing immediately under a name taken from its URL, and the real title, artist, album
and cover replace it a moment later. If the file has no embedded picture, `cover.jpg` beside
it on the server is tried once. Live streams are skipped entirely -- they have no tags, and
fetching their head would only compete with the audio for bandwidth.

Remote folders are deliberately **not** added to `library.idx`: indexing them would mean an
HTTP request per file for tags. They browse live instead, so what you see is always current.
That also means albums/artists/genres cover the card only.

### How a stream is identified

A station URL rarely looks like a file, so three things are tried in order:

1. **The server's `Content-Type`.** `audio/mpeg` → MP3, `audio/flac` → FLAC, `audio/aac` →
   AAC, and so on. This is decisive: `http://live.powerhitz.com/hot108` has no extension at
   all, and `…/hot108?aw_0_req.gdpr=true` would otherwise look like it ended in `.gdpr=true`.
2. **The extension**, with any query string stripped first.
3. **The bytes.** An Icecast connection starts wherever the encoder happens to be, so the MP3
   probe scans a 1 KB window for a frame sync and confirms it by checking that a second sync
   appears exactly where the first frame's header says it should — a lone `0xFF` is not
   enough.

When the type is recognised but not compiled in, the error names it (`… is AAC, which is not
compiled in`) rather than just saying unsupported.

### Playlist URLs

Stations usually publish a `.m3u`/`.pls` that merely *contains* the stream address — for
example `https://stream.zeno.fm/…​.m3u` holds one line pointing at the real endpoint. Those
are fetched and parsed automatically, and when the file lists several mirrors each is tried
in turn until one connects. Plain M3U, `#EXTM3U` and PLS (`File1=…`) are understood; an HLS
manifest is detected by its `#EXT-X-` tags and rejected with a clear message instead of being
half-played.

Redirects are followed, including the 303/307/308 that CDNs use to hand out signed
per-session URLs.

### Buffering a stream

Streaming servers front-load. Measured against `live.powerhitz.com/hot108`:

| window | received | average rate |
| --- | --- | --- |
| first 0.5 s | 12 KB | — |
| first 2 s | **560 KB** | 2239 kbps |
| first 11 s | 681 KB | 495 kbps, settling to 128 kbps |

Roughly 35 seconds of audio arrives in the first two seconds, then the server throttles to
real time. That matters twice over. A reserve smaller than the burst forces the reader to
stall, and a stalled client is what Icecast drops off its send queue — which is how a stream
that seems to be buffering fine ends up reconnecting every minute.

So a URL gets a much larger reserve than a file on the card, sized to swallow the burst:

| | LOW | NORMAL | HIGH |
| --- | --- | --- | --- |
| Card reserve | 64 KB | 128 KB | 256 KB |
| **Network reserve** | **256 KB** | **512 KB** | **1 MB** |
| ≈ at 128 kbps | 16 s | 32 s | 64 s |

Reads are issued in 4 KB pieces rather than 16 KB, because
`esp_http_client_read_response()` blocks until the requested length is satisfied — at a
stream's real-time rate a 16 KB read is a full second per ring update, and a stall takes that
long to notice.

Playback starts once the compressed stage holds a few seconds (`prebuffer_ms`, 2.5–4 s by
profile) rather than on the PCM stage alone, and the *Buffering* state is only re-entered
when **both** stages are dry. Watching the PCM level by itself made the state flap between
Buffering and Playing on every dip.

### Delayed live playback

The buffering above still starts playing within a few seconds, so the reserve it has banked is
whatever happened to arrive by then. On a link that stalls for longer than that, the audio
drops out. **Settings → Live stream buffer** trades latency for immunity:

| setting | behaviour |
| --- | --- |
| `Live` (default) | Play as soon as the normal prebuffer is met — lowest latency |
| `12 s` / `15 s` / `20 s` | Hold output silent until that much of the broadcast is banked |

With a delay set, playback runs that far behind the live edge and the stream has to be
unreachable for longer than the delay before anything is audible. Output is genuinely paused
while the reserve fills — otherwise the audio task would drain the PCM stage as fast as the
decoder filled it and nothing would accumulate. The decoder keeps running, stops when the PCM
stage is full, and the compressed reserve grows behind it. Progress shows as `Buffering 8s/20s`.

The ring is sized from the setting rather than the profile alone. It is planned for 128 kbps —
what almost all radio uses — and deliberately tightly, because the ring rounds up to a power of
two and a more generous estimate would double it on every profile for a setting most listeners
leave off. Resulting reserve:

| delay | LOW | NORMAL | HIGH |
| --- | --- | --- | --- |
| `Live` | 256 KB | 512 KB | 1 MB |
| `12 s` | 256 KB | 512 KB | 1 MB |
| `15 s` / `20 s` | 512 KB | 512 KB | 1 MB |

A stream above 128 kbps banks less than the requested delay rather than failing, and the trim is
logged. If PSRAM is too tight for the ring at all, `media_ring_create()` falls back to a quarter
of it and the pre-roll target follows the capacity it actually got.

If the server stops sending mid-fill, the wait is abandoned after 8 s without progress and
playback starts with whatever was banked — any forward progress restarts that clock, so a
merely slow link still gets its full reserve.

The setting applies from the next stream opened, not to one already playing.

### Live streams

A response with no `Content-Length` is treated as a broadcast: duration and seeking are
disabled, and Now Playing shows `LIVE` with a flat bar rather than a fake playhead.

`Icy-MetaData: 1` is always requested, so Icecast/Shoutcast stations that send inline
`StreamTitle` updates drive the Now Playing text. `Artist - Title` is split so the layout
matches a local file. The metadata blocks are stripped inside the source layer, which is why
the decoders need no knowledge of any of this.

`icy-name` and `icy-genre` are applied to the album and genre lines **as soon as the response
headers arrive**, not on the first inline title — a station may not send one for minutes, and
some never do.

Cover art for a broadcast comes only from the station: `StreamArtwork`, or `StreamUrl` when it
points at an image. Most stations send neither, and then a stream keeps the placeholder. A
broadcast is never looked up like a file: there are no tags in the middle of a stream, and
asking would download its head again and again while competing with the audio for bandwidth.

A dropped connection is retried up to four times with a widening delay. For a seekable HTTP
*file* the retry resumes mid-file with a `Range` header; a broadcast simply reconnects.

### What this reuses

Network support added no new HTTP stack: `rg_network_http_*` gained request headers and a
response-header callback, and `media_source` gained a second backend behind the same
`read`/`seek`/`tell`/`eof` interface the file backend already implemented. Everything above
it — decoders, ring buffers, EQ, visualiser, queue, playlists — is unchanged.

---

## 5. Theming

The Media tab is named `mediaplayer`, so a theme can supply:

```
/retro-go/themes/<name>/
    logo_mediaplayer.png         46 x 50    magenta = transparent
    banner_mediaplayer.png       272 x 24   magenta = transparent
    background_mediaplayer.png   320 x 240
```

All three are also built in, so the tab looks right with no theme installed.

The player's own chrome reads a `media` section from `theme.json`:

| Key | Used for |
| --- | --- |
| `background` | Page background when no album-art background is shown |
| `surface` | Cards, panels, header and footer bands |
| `text` / `text_dim` | Primary and secondary text |
| `divider` | Rules, empty progress track, scrollbar trough |
| `accent` | Progress fill, selection, transport, spectrum bars |
| `accent_dim` | Muted accent |
| `highlight` | Peak markers, favourites, the bright end of the spectrum ramp |

With **Dynamic theme** on (the default), `accent`, `accent_dim`, `highlight` and `surface`
are replaced per track by colours extracted from the album art; everything else in the
section still applies. Turn it off to keep a palette exactly as written.

---

## 6. Architecture

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

| Task | Priority | Core | Stack | Purpose |
| --- | --- | --- | --- | --- |
| `media_audio` | 8 | `RG_TASK_AFFINITY_AUDIO` | 4 KB | Drain PCM into I2S |
| `media_dec` | 7 | `RG_TASK_AFFINITY_AUDIO` | 22 KB | Decode into PCM |
| `media_io` | 4 | `RG_TASK_AFFINITY_IO` | 4 KB | SD prefetch |
| `media_scan` | 1 | `RG_TASK_AFFINITY_MAIN` | 8 KB | Library indexing |
| `media_art` | 1 | `RG_TASK_AFFINITY_MAIN` | 8 KB | JPEG/PNG decode |

The launcher enables `RG_SEPARATE_DISPLAY_AUDIO`: display scaling (priority 6) and SPI
completion (7) use `RG_TASK_AFFINITY_DISPLAY`, which defaults to the UI/main core for this
application. On the eight ESP32-S3 DIY targets this puts rendering on core 0 and decoding,
PCM output and the I2S writer on core 1. Other applications retain their target's existing
display affinity. Single-core ports cannot provide physical core isolation.

`rg_task_create()` uses FreeRTOS `xTaskCreatePinnedToCore()` on ESP-IDF. Media output (8)
and decoding (7) sit below the shared I2S writer (9). Source reads, ring semaphores and
I2S back-pressure block these tasks when there is no work; they never busy-wait at high
priority. Task priority defaults may be overridden with `MEDIA_AUDIO_TASK_PRIORITY` and
`MEDIA_DECODE_TASK_PRIORITY`. The audio task performs no rendering, filesystem reads,
metadata parsing or FFT transforms.

`media_dec` needs an unusually large stack because minimp3 places a ~16 KB
`mp3dec_scratch_t` (bit reservoir, granule buffers, synthesis state) on the stack inside
`mp3dec_decode_frame`. Each task logs its remaining stack headroom when it exits, so the
sizes can be checked against real material rather than guessed at.

Because the player runs up to five tasks at once, `rg_task_t tasks[]` in `rg_system.c` was
raised from 8 to 16 slots; with `rg_display`, `rg_input` and `rg_sysmon` already resident the
old table had exactly one free slot left and a rescan-while-playing would have hit
`RG_ASSERT(task, "Out of task slots")`.

The UI reads a `media_snapshot_t` captured once per frame and posts commands. Decoder
publication and detachment use a short mutex; snapshot readers return the previous
snapshot when that mutex is busy. File/network opens and closes happen outside it.
Track metadata is copied into UI-owned storage, and workers coalesce notifications for
delivery by `media_player_tick()` on the UI task.

PCM output is held during initial buffering and starvation recovery. The target follows
the ring's actual capacity when memory allocation falls back to a smaller ring. Prebuffer
and live pre-roll checks also run while a decoded block waits for space, preventing a
paused/full-ring deadlock. Short files and EOF bypass the normal startup threshold.

Flush and sample-rate changes serialize with PCM chunk processing. EQ controls publish
pending settings that the output task applies between blocks; only that task changes
coefficients and filter history. The visualizer tap uses internal RAM and atomic sample
publication; FFT analysis remains on the UI core. At low buffer levels the UI limits itself
to 15 FPS and defers FFT analysis. Dimmed screens also skip FFT work.

### Playback position

Progress comes from PCM frames submitted to the output pipeline
(`media_audio_position_ms()`), rather than elapsed wall time. Frame counters and seek bases
are read together under the output mutex to prevent torn 64-bit reads on the 32-bit MCU.
The counter leads physical playback by the bounded software/I2S DMA reserve. At EOF, a
silent DMA block completes the final partial block, and the controller waits for the
reserve to play out before changing track/rate. Padding never advances track position.

### Resource manager

`media_player_pressure()` returns 0 (idle), 1 (playing comfortably) or 2 (a buffer is
running low). Both the library scanner and the artwork worker consult it before every unit
of work and back off at level 2. Audio continuity always wins.

---

## 7. Memory profiles

Selected at runtime from the detected PSRAM size (`media_profile.c`).

| | LOW (≤2 MB, N8R2) | NORMAL (2–6 MB) | HIGH (>6 MB, N16R8) |
| --- | --- | --- | --- |
| Compressed reserve (card) | 64 KB | 128 KB | 256 KB |
| Compressed reserve (network) | 256 KB | 512 KB | 1 MB |
| PCM ring | 8 K frames (32 KB) | 16 K frames (64 KB) | 32 K frames (128 KB) |
| Prebuffer | 2 K frames | 4 K frames | 8 K frames |
| Artwork cache | 192 KB / 4 entries | 512 KB / 8 | 1.5 MB / 16 |
| Cover size | 160 px | 200 px | 260 px |
| FFT | 128 pt / 16 bands | 256 / 20 | 512 / 24 |
| Target FPS | 30 | 30 | 60 |
| Blurred background | off | on | on |
| Second-decoder memory budget | no | yes | yes |
| Particle visualizer | no | no | yes |
| Resident index | 4 K tracks max | 8 K | 20 K |

PSRAM holds the ring buffers, artwork, the library index and decode scratch. Internal SRAM
holds only the small, latency-sensitive pieces: the audio output chunk and the TJpgDec work
pool. Every allocation that can fail uses `MEM_NOPANIC` and has a defined fallback.

Only a 24-byte entry plus a display-name pool is resident per track; full records are read
back from `library.idx` by record number through one persistent file handle. That is what
keeps a library far larger than PSRAM usable on an N8R2.

---

## 8. Settings

All under the `launcher` namespace with a `Media.` prefix, stored via
`rg_settings_*` (NVS-backed). Nothing large is ever written to NVS.

Background playback · Resume playback · Remember queue · Scan on startup · Normalization ·
Gapless · Crossfade · Skip failed tracks · **Live stream buffer** · Album-art background ·
Dynamic theme · Low effects · Visualizer + FPS · Lyrics + offset · EQ enable/preset/7 band
gains · Sleep timer · Debug overlay · Media root.

Play statistics are debounced (30 s) and written atomically; favourites are written
immediately because they are an explicit user action.

---

## 9. Audio focus and emulators

`media_audio_acquire()`/`release()` model ownership of the shared I2S device
(`NONE`/`PLAYER`/`EMULATOR`/`SYSTEM`). Two subsystems can never drive I2S at once.

`application_start()` in the launcher calls `media_suspend_for_app()` before
`rg_system_switch_app()`: the queue is saved, playback stops, and the hardware is handed
back with its original sample rate restored. Emulator audio always wins.

Background playback (default **Launcher only**) keeps music going while browsing the
launcher, and is dropped the moment an emulator is launched.

---

## 10. Build configuration

Feature flags live in `media_config.h` and can be overridden from the build system or a
target `config.h`:

```
MEDIA_PLAYER_ENABLE   MEDIA_CODEC_WAV   MEDIA_CODEC_MP3   MEDIA_CODEC_FLAC
MEDIA_CODEC_AAC       MEDIA_CODEC_OGG   MEDIA_CODEC_OPUS
MEDIA_EQ_ENABLE       MEDIA_FFT_ENABLE  MEDIA_LYRICS_ENABLE  MEDIA_ARTWORK_ENABLE
MEDIA_DEBUG_STATS
```

Launcher partitions are `0x1C0000` on N16R8 and `0x170000` on N8R2 targets.
The full release packer checks these actual budgets; the standalone IDF build uses a larger
dummy partition, so its size check alone is insufficient (see each target's `env.py`).

---

## 11. Changes made to Retro-Go core

Adding a long-running foreground app to the launcher exposed a few things in
`components/retro-go` that only misbehave once several tasks are busy at the same time.

**`screen_timeout_tick()` now only runs on the UI task.** It hangs off `rg_task_delay()`,
which every task in the firmware calls, and waking the screen dispatches `RG_EVENT_REDRAW` —
making the application repaint its entire UI on whatever stack happened to call
`rg_task_delay()` first. With the player's five tasks running, `rg_sysmon` (3 KB) won that
race and overflowed. The tick also polled input and read settings from arbitrary tasks
concurrently. It is now pinned to the task that called `rg_system_init()`.

**The launcher no longer repaints over the player.** `event_handler()` checks
`media_is_foreground()` before calling `gui_redraw()`.

**The WS2812 status LED had two bugs.**

`rmt_tx_wait_all_done()` takes **milliseconds** and applies `pdMS_TO_TICKS()` internally, but
it was being passed `pdMS_TO_TICKS(100)` — a double conversion. At this firmware's 100 Hz
tick rate that became a 10 ms wait, short enough to expire occasionally; and the value is now
passed in milliseconds.

Worse, a single expiry wedges the channel forever: `esp_driver_rmt` returns from a timed-out
wait *before* decrementing `num_trans_inflight`, so every later wait needs one more
completion than will ever arrive. That counter lives inside the channel object, which is why
disabling and re-enabling cannot clear it — the channel is now destroyed and rebuilt, and
after eight consecutive failures the LED is switched off rather than rebuilt once a second
forever.

Separately, LED updates are coalesced to one per 15 ms. The SD transaction hook toggles the
disk-activity indicator around *every* block transfer, and each toggle is a blocking RMT
transmit plus a 300 µs latch delay held under the LED mutex; streaming audio from the card
turned that into a constant stream of WS2812 frames on the same lock the IO task needs.

**`rg_task_t tasks[]` grew from 8 to 16.** See the note in §4.

**`CONFIG_I2S_SKIP_LEGACY_CONFLICT_CHECK=y`** on the six ESP32-S3 targets removes the
`legacy i2s driver is deprecated` notice printed on every boot. Note this also disables
IDF's abort when both I2S drivers are linked; if a component using `driver/i2s_std.h` is
ever added, turn it back on to get that diagnostic. Actually migrating
`drivers/audio/i2s.c` to `i2s_std` is the real fix, but that driver carries headphone-jack
detection and dual-DAC routing that need hardware to validate.

---

## 12. Known limitations

* **AAC/M4A, Ogg Vorbis and Opus have no decoder.** Their tags, duration and artwork parse
  correctly and the codec registry already knows about them, so adding a `codec_*.c` is the
  only work required. Files in these formats currently report *Unsupported format*.
* **Gapless transitions and crossfade are not implemented.** The settings controls have
  been removed and saved values are ignored. WAV/FLAC decoders do not add codec delay, but
  opening the next track only after draining still introduces a transition gap. MP3 also
  needs encoder/decoder-delay compensation. A next-track decoder and sample-accurate
  transition/mixing path are required before these features can be advertised.
* **Waveform overview** (`waveform_overview` in the profile) is reserved but not generated;
  the seek bar is linear.
* **Remote listings block the UI while they load.** A "Connecting..." message is shown, but a
  slow or unreachable server stalls the browser for up to the 8 second timeout.
* **No SMB/CIFS, FTP or UPnP/DLNA.** HTTP and WebDAV only.
* **AAC/HE-AAC stations will not play.** A good share of internet radio is AAC+; the format is
  identified and named in the error, but no decoder is vendored. See the AAC row in §3.
* **HLS (`#EXT-X-`) is not supported** — it is a segmented protocol, not a container.
* **Search** is not implemented — the input hardware has no usable text entry, so the browser
  offers category, folder, album, artist and genre navigation instead.
* **Bookmarks** for long recordings are not implemented; the resume-position mechanism
  (automatic for files longer than 15 minutes) covers the common podcast/audiobook case.
* **RTL shaping** is not performed. UTF-8 metadata is preserved byte-exact and truncation is
  always codepoint-aligned, and all text layout goes through `media_ui_draw_marquee`, so
  bidi/shaping can be added in one place later. Glyphs the current font lacks render as the
  font's replacement rather than corrupting the string.


## 13. Validation and release readiness

`python tools/test_media_audio.py --cc /path/to/clang-or-zig` compiles the production ring,
PCM output, EQ and FFT implementations and the controller's actual buffering helpers with
a mocked sink. It checks 64 physical/cursor wrap cases, a 2 MB concurrent producer/consumer
transfer, startup buffering, dry pause, starvation episodes, FIFO delivery, final partial
chunks and DMA padding, position/seek bases, stop-before-task-entry, six EQ sample rates,
all FFT sizes, network prebuffer, live pre-roll, low-memory buffer fallback and short-file
startup. CI and release workflows run these tests with the native C compiler.

Background housekeeping is polled at 10 Hz from the launcher UI loop, so sleep timers,
play statistics, resume positions, station titles and headphone-disconnect pause continue
when the player screen is closed. The existing `pause_on_unplug` preference is now exposed
in settings and acted on when the route changes from headphones to speaker. Background
playback no longer allocates lyrics or requests artwork; those resume on foreground entry.
The host harness also verifies foreground exclusion, background polling frequency,
headphone-disconnect behavior and display/audio affinities for all eight release targets
with both launcher and emulator defaults.

ESP-IDF 5.5.5 launcher builds are checked for N16R8 ILI9341 and N8R2 ST7796,
including their actual target partition budgets. N8R2 launcher PNG code uses size
optimization to preserve flash headroom; audio/decoder optimizations are retained. These host
checks and compilation do not measure real DMA deadlines, analog noise or task stack usage.
Before release, test MP3/WAV/FLAC at 8/32/44.1/48 kHz on both an N8R2 and N16R8 device:

- Run full-screen visualizers and artwork, browse large libraries and rescan during playback.
  Compare buffer levels and underrun counts before/after sustained CPU and SD load.
- Exercise rapid seek/next/previous/pause, short tracks, mixed sample-rate queues, corrupt
  and missing files, repeat-one with failed tracks, and headphone/speaker switching.
- Disconnect/reconnect streaming Wi-Fi, interrupt live pre-roll, dim/wake the display,
  leave/re-enter background playback, expire the sleep timer and launch an emulator.
- Confirm the final audio fragment is audible, no old PCM survives beyond the hardware
  reserve on a seek, stack headroom remains positive and repeated sessions reclaim memory.

Known format, networking and transition limitations in section 12 remain. Production
readiness requires device results; priority/core isolation cannot compensate for sustained
throughput below the track's sample rate or for a stalled network longer than the reserve.
