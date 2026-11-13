# Theme artwork sources

The new small launcher glyphs, banners, fallback backgrounds and representative
previews are authored as SVG templates in `tools/theme_assets.py` and rasterized
with resvg. That script is their editable source. Existing artwork is retained;
flat or fully color-keyed banner/logo placeholders are replaced. Magenta is the
firmware's color key, because its PNG loader decodes to RGB24 rather than alpha.

`classic-background.png` is the original full-resolution image generated with
the built-in image generation tool. Production copies in `themes/classic/`
are exported at 320×240. No API CLI fallback was used.

Final prompt:

> Create a finished raster background asset for the existing Retro-Go 'classic'
> handheld emulator launcher theme. Landscape 4:3 aspect ratio, intended final
> size 320 by 240 pixels, clean and legible on small LCD. Sophisticated deep
> midnight navy (#080C25) with very subtle cobalt and pale periwinkle (#8FB0FF)
> luminous outlines of generic retro handheld silhouettes and circuit traces
> confined to the far right quarter and lower corners. Most of the left and
> center must be calm dark negative space for a game list and system selection
> card. Understated 1990s console dashboard atmosphere, crisp restrained geometry,
> soft restrained blue glow, no busy stars, no gradients banding, no text, no
> letters, no logos, no UI mockup, no frames, no game characters. The entire
> canvas is the background, full bleed, polished pixel-friendly composition.

Rebuild missing assets with `python tools/theme_assets.py --complete` after
installing `requirements-tools.txt`. Existing nonblank tab art is not overwritten.
Previews and palette readability fixes are refreshed. Validation alone uses
`python tools/theme_assets.py --check` and does not modify any files.

Artwork is installed on SD rather than compiled into firmware. After editing or
completing themes, run `python tools/gen_images.py` and copy the exported
`build/sdcard-assets/retro-go/` folder to the SD root. Release SD archives include
the same files automatically; a firmware rebuild is unnecessary for theme edits.

The completeness pass added 497 missing tab assets and replaced 124 blank
banner/logo placeholders across 15 themes. Each bundle includes 23 tab sets,
`background.png`, `preview.png`, and a full `theme.json`. Secondary text meets
a 4.5:1 contrast threshold against its configured surface after RGB565
quantization. Theme hues and existing nonblank tab artwork remain intact.

`themes/PREVIEW.html` displays representative GBA library previews, not captured
firmware screenshots. The previews use the actual theme backgrounds, logos and
palette values; inspect them on a device for final LCD/color assessment.
