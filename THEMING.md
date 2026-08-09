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

Colors are RGB565 and can be represented as integers or hex strings. The special value `transparent` is also accepted in some places.

<details>
  <summary>View example theme.json</summary>

````json
{
    "description": "Default Retro-Go Theme",
    "website": "https://github.com/ashkan89/retro-go/",
    "author": "ducalex",
    "dialog": {
        "__comment": "This section contains global dialog colors",
        "background": "0x0010",
        "foreground": "0xFFFF",
        "border": "0x6B4D",
        "header": "0xFFFF",
        "scrollbar": "0xFFFF",
        "shadow": "none",
        "item_standard": "0xFFFF",
        "item_disabled": "0x8410",
        "item_message": "0xBDF7"
    },
    "launcher_1": {
        "__comment": "This section contains launcher colors variant 1",
        "background": "0x0000",
        "foreground": "0xFFDE",
        "list_standard_bg": "transparent",
        "list_standard_fg": "0x8410",
        "list_selected_bg": "transparent",
        "list_selected_fg": "0xFFFF"
    },
    "launcher_2": {
        "__comment": "This section contains launcher colors variant 2",
        "background": "0x0000",
        "foreground": "0xFFDE",
        "list_standard_bg": "transparent",
        "list_standard_fg": "0x8410",
        "list_selected_bg": "transparent",
        "list_selected_fg": "0x07E0"
    },
    "launcher_3": {
        "__comment": "This section contains launcher colors variant 3",
        "background": "0x0000",
        "foreground": "0xFFDE",
        "list_standard_bg": "transparent",
        "list_standard_fg": "0x8410",
        "list_selected_bg": "0xFFFF",
        "list_selected_fg": "0x0000"
    },
    "media": {
        "__comment": "This section contains media player colors",
        "background": "0x0000",
        "surface": "0x10A3",
        "text": "0xEF5E",
        "text_dim": "0x8C51",
        "divider": "0x31C6",
        "accent": "0x5D5F",
        "accent_dim": "0x3314",
        "highlight": "0xA65F"
    },
    "launcher_4": {
        "__comment": "This section contains launcher colors variant 4",
        "background": "0x0000",
        "foreground": "0xFFDE",
        "list_standard_bg": "transparent",
        "list_standard_fg": "0xAD55",
        "list_selected_bg": "0xFFFF",
        "list_selected_fg": "0x0000"
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

### Images

It is highly recommended to keep the image files sizes as small as possible to ensure good loading speed. This can be achieved by using the lowest bit depth possible when saving your PNG file. Tools like [pngquant](https://pngquant.org/) can also help!

Magenta (rgb(255, 0, 255) / 0xF81F) is used as the transparency color in some situations.
