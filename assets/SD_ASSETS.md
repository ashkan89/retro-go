# SD assets and embedded data

Install the release's `sdcard_structure.zip` at the SD root before updating
firmware. Merge it into the existing directory tree. For a local checkout:

```console
python tools/gen_images.py --output build/sdcard-assets
```

Copy `build/sdcard-assets/retro-go/` to the SD root. The result includes:

| SD path | Purpose |
| --- | --- |
| `/retro-go/themes/default/` | Default launcher PNGs and colors |
| `/retro-go/themes/<theme>/` | Other complete selectable themes |

All 64 PNG arrays from the former `launcher/main/images.c` were decoded and
compared byte for byte with the default theme files: 81,480 bytes in total.
They are no longer linked into the launcher. The bundle contains all 1,202 PNGs
and 15 theme configurations, including preview images and all 23 tab sets.

The launcher first tries the selected theme, then the default theme on SD.
Missing artwork uses text labels and the configured background colors.
Low-memory mode continues to disable artwork. The full web manager is now
compressed into launcher flash and works even with no SD card. Any old
`/retro-go/webui/index.html` is ignored and can be removed. Boot and recovery
dialogs do not require artwork.

## Embedded data audit

| Data | Action and reason |
| --- | --- |
| Launcher PNG arrays | Removed; exact copies installed on SD |
| Web manager | Embedded as deterministic gzip in launcher flash; no SD dependency |
| Hourglass RGB565 pixels, 4,608 bytes | Removed; drawn procedurally with a 96-byte stack row buffer |
| Bitmap fonts, including Arabic/Persian | Kept for menus, startup, SD errors and recovery; no runtime SD font loader exists |
| Small update/status icon | Kept so update/recovery status works without SD |
| Emulator ROM/BIOS replacements and lookup tables | Kept as executable emulation dependencies |
| Game cover art and album artwork | Already read from storage or media files |

Theme changes do not require a firmware rebuild. Web manager changes rebuild
the launcher automatically. For old
generated artwork files, `python tools/gen_images.py --extract old-images.c
--output build/recovered-artwork` recovers their PNGs without executing C code.
The extractor checks filenames, PNG signatures and declared sizes, and refuses
to overwrite an existing file with different bytes.

Measured with cached ESP-IDF 5.5.5 release builds for S3 N8R2 ST7796:

| Application | Before | After | Flash saved |
| --- | ---: | ---: | ---: |
| Launcher | 1,559,264 bytes | 1,466,944 bytes | 92,320 bytes |
| Retro-core (Nofrendo) | 1,180,048 bytes | 1,175,616 bytes | 4,432 bytes |

Both applications now fit their nominal 8 MB target reservations. Other apps
that use the shared busy indicator also benefit; their exact savings depend on
the compiler and target. Fonts, emulator code and partition budgets are unchanged.

Run `python tools/theme_assets.py --check` to validate theme completeness and
`python tools/test_sd_assets.py --cxx c++` to exercise the production artwork
lookup and busy indicator on a host. Run `python tools/test_webui.py --cc cc`
for flash serving, path validation, staged uploads and download checks.
