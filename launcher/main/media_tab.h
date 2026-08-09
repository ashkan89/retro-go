#pragma once

/** Register the Media tab alongside the emulator tabs. */
void media_tab_init(void);

/** Rebuild the tab's rows (playback state changed, or we just came back from the player). */
void gui_invalidate_media_tab(void);
