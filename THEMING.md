# Theming Retro-Go

This document should document what are themes, how they're structured, and how to make them.

Example themes can be found in the [themes](/themes/) folder of this project.


## Theme Structure

A theme is a folder placed in `sd:/retro-go/themes` containing the following files:

````
/retro-go/themes
└── example
    ├── background.png
    ├── background_*.png
    ├── banner_*.png
    ├── logo_*.png
    ├── preview.png
    └── theme.json
````

| Name | Format | Description | Required |
|--|--|--|--|
| `theme.json` | JSON | Contains the theme metadata (description, author, colors, etc) | Yes |
| `preview.png` | PNG 160x120 | Theme preview to be displayed in the theme selector | No |
| `background.png` | PNG 320x240 | Launcher's default background | No |
| `background_<tab_name>.png` | PNG 320x240 | Launcher's per-tab backgrounds | No |
| `banner_<tab_name>.png` | PNG 272x24 | Launcher's per-tab banners | No |
| `logo_<tab_name>.png` | PNG 46x50 | Launcher's per-tab logos | No |

The media player's tab is named `mediaplayer`, so its images are `background_mediaplayer.png`,
`banner_mediaplayer.png` and `logo_mediaplayer.png`.


### theme.json

The `theme.json` file contains the colors used by the theme, as well as some author meta data.

All fields are optional but it is not recommended to omit individual values because the fallback value that will be used is subject to change between retro-go versions. You are free to omit entire sections, however. For example if you only wish to retheme the launcher but not the dialogs, or vice-versa.

Colors can be written as RGB565 (`0xFFFF`, integer or hex string) or, more readably, as RGB888
(`0xFFFFFF` - any 8-character hex string is treated as 24-bit and converted). The special value
`transparent` is also accepted in some places, and `none` means "do not draw this at all".

The themes shipped in this repository each use one hue as their identity (indigo, magenta,
lime, orange, monochrome, cyan, amber, navy, mint, azure, crimson, violet, yellow) and derive
the rest of their palette from it, which is a good starting point for a new theme: pick a
background and an accent first, then tint everything else towards them.

<details>
  <summary>View example theme.json</summary>

````json
{
    "description": "default",
    "website": "https://github.com/ashkan89/retro-go/",
    "author": "ducalex",
    "dialog": {
        "__comment": "Global dialog colors. Colors may be given as RGB565 (0xFFFF) or RGB888 (0xFFFFFF). The accent/surface/divider keys drive the cards, header chips, selection pills and scrollbars; leave any of them out and it is derived from the keys above it.",
        "background": "0x11121A",
        "foreground": "0xF8FCFF",
        "border": "0x505967",
        "header": "0xF8FCFF",
        "scrollbar": "0x5AAAFF",
        "shadow": "0x000000",
        "item_standard": "0xE1E8F1",
        "item_disabled": "0x6E727A",
        "item_message": "0xB9BFC8",
        "item_value": "0x9A9FA8",
        "accent": "0x5AAAFF",
        "accent_dim": "0x2D4C71",
        "highlight": "0xC0DFFF",
        "surface": "0x333844",
        "surface_alt": "0x1B273A",
        "divider": "0x45464C",
        "text_dim": "0x888D96"
    },
    "launcher_1": {
        "__comment": "Launcher variant 1: white selection over the accent pill",
        "background": "0x0D0E14",
        "foreground": "0xF8FCFF",
        "list_standard_bg": "transparent",
        "list_standard_fg": "0x969BA3",
        "list_selected_bg": "transparent",
        "list_selected_fg": "0xFFFFFF"
    },
    "launcher_2": {
        "__comment": "Launcher variant 2: selection uses the theme's second color",
        "background": "0x0D0E14",
        "foreground": "0xF8FCFF",
        "list_standard_bg": "transparent",
        "list_standard_fg": "0x8D929A",
        "list_selected_bg": "transparent",
        "list_selected_fg": "0x7BE8A0"
    },
    "launcher_3": {
        "__comment": "Launcher variant 3: solid accent pill with dark text",
        "background": "0x0D0E14",
        "foreground": "0xF8FCFF",
        "list_standard_bg": "transparent",
        "list_standard_fg": "0x969BA3",
        "list_selected_bg": "0x5AAAFF",
        "list_selected_fg": "0x11121A"
    },
    "launcher_4": {
        "__comment": "Launcher variant 4: high contrast, white pill with black text",
        "background": "0x0D0E14",
        "foreground": "0xF8FCFF",
        "list_standard_bg": "transparent",
        "list_standard_fg": "0xB5BBC3",
        "list_selected_bg": "0xFFFFFF",
        "list_selected_fg": "0x000000"
    },
    "media": {
        "__comment": "Media player colors. Accents follow the album art when Dynamic theme is on.",
        "background": "0x0B0C11",
        "surface": "0x333844",
        "text": "0xEEF6FF",
        "text_dim": "0x888D96",
        "divider": "0x45464C",
        "accent": "0x5AAAFF",
        "accent_dim": "0x376191",
        "highlight": "0xC0DFFF"
    }
}
````
</details>


### The `media` section

The media player draws its own chrome rather than reusing the launcher's list colors:

| Key | Used for |
|--|--|
| `background` | Page background, when no album art background is shown |
| `surface` | Cards, panels, the header and footer bands |
| `text` | Primary text |
| `text_dim` | Secondary text, times, hints |
| `divider` | Rules, empty progress track, scrollbar trough |
| `accent` | Progress fill, selection, transport, spectrum bars |
| `accent_dim` | Muted accent (equalizer band rules, folder glyphs) |
| `highlight` | Peak markers, favourites, the brighter end of the spectrum ramp |

When *Dynamic theme* is enabled in the player's settings, `accent`, `accent_dim`,
`highlight` and `surface` are replaced by colors extracted from the current album art;
the rest of the section always applies. Turn *Dynamic theme* off to keep your palette
exactly as written.

### The `dialog` section

Dialogs, the in-game menu, the on-screen keyboard, the file picker and the launcher's chrome are
all drawn from this one section. Everything is optional: a key that is missing is derived from the
keys above it, so a theme written before these existed still gets a coherent look (its own
background tinted for the surfaces, its own text dimmed for secondary text) rather than colors
that clash with it.

| Key | Used for |
|--|--|
| `background` | The card surface behind a dialog, and the in-game status bands |
| `header` | Dialog title text |
| `border` | Hairline around a card |
| `item_standard` | Selectable row text |
| `item_disabled` | Rows that cannot be selected |
| `item_message` | Message text (alerts, confirmations) |
| `item_value` | The value shown on the right of a row |
| `accent` | Selection bar, header chip bar, scrollbar thumb, Wi-Fi bars, charging battery |
| `accent_dim` | Fill of the selected row's pill |
| `highlight` | Text on the selected row |
| `surface` | Panels and chips that sit on top of the background (also the launcher's bands) |
| `surface_alt` | Header chips, badges, keyboard keys |
| `divider` | Rules, scrollbar trough, empty progress track |
| `text_dim` | Secondary text and hints |
| `shadow` | Card drop shadow. Set to `none` to switch shadows off |
| `scrollbar` | Scrollbar thumb (defaults to `accent`) |

The battery fill (green/amber/red) and the save-slot frame (accent when a state exists, red when
the slot is empty) are deliberately *not* themeable: they carry meaning, and a low battery has to
stay recognisable in every theme.

### Images

It is highly recommended to keep the image files sizes as small as possible to ensure good loading speed. This can be achieved by using the lowest bit depth possible when saving your PNG file. Tools like [pngquant](https://pngquant.org/) can also help!

Magenta (rgb(255, 0, 255) / 0xF81F) is used as the transparency color in some situations.
