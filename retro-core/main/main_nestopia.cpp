#include "shared.h"

#include <NstApiEmulator.hpp>
#include <NstApiFds.hpp>
#include <NstApiInput.hpp>
#include <NstApiMachine.hpp>
#include <NstApiNsf.hpp>
#include <NstApiSound.hpp>
#include <NstApiVideo.hpp>
#include <NstApiUser.hpp>
#ifdef RG_NESTOPIA_PROFILE
#include <NstMachine.hpp>
#include <NstApiCartridge.hpp>
#include <rg_nestopia_profile.hpp>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <istream>
#include <new>
#include <streambuf>
#include <string>

static Nes::Api::Emulator *activeEmulator;
static Nes::Api::Machine *activeMachine;
static Nes::Api::Video *activeVideo;
static rg_app_t *app;
static rg_surface_t *updates[2];
static rg_surface_t *currentUpdate;
static bool nsfPlayer;
static int overscan = true;
static int autocrop;
static int palette;
static uint32_t previousNsfButtons;
static int refreshRate;

static int16_t audioMono[AUDIO_BUFFER_LENGTH];
static rg_audio_frame_t audioStereo[AUDIO_BUFFER_LENGTH];

static const char *SETTING_AUTOCROP = "autocrop";
static const char *SETTING_OVERSCAN = "overscan";
static const char *SETTING_PALETTE = "nestopia_palette";
static const char *SETTING_SPRITELIMIT = "spritelimit";

class MemoryStreamBuffer : public std::streambuf
{
protected:
    pos_type seekoff(off_type offset, std::ios_base::seekdir direction,
                     std::ios_base::openmode mode) override
    {
        if (!(mode & std::ios_base::in))
            return pos_type(off_type(-1));
        off_type base = direction == std::ios_base::beg ? 0 :
                        direction == std::ios_base::cur ? gptr() - eback() : egptr() - eback();
        const off_type length = egptr() - eback();
        if (offset < -base || offset > length - base)
            return pos_type(off_type(-1));
        setg(eback(), eback() + base + offset, egptr());
        return pos_type(base + offset);
    }

    pos_type seekpos(pos_type position, std::ios_base::openmode mode) override
    {
        return seekoff(off_type(position), std::ios_base::beg, mode);
    }

public:
    MemoryStreamBuffer(const void *data, size_t size)
    {
        char *begin = const_cast<char *>(static_cast<const char *>(data));
        setg(begin, begin, begin + size);
    }
};

// Keep native saves separate from Nofrendo's SRAM and state formats.
static void NST_CALLBACK file_io(void *, Nes::Api::User::File &file)
{
    using File = Nes::Api::User::File;
    const char *suffix;
    bool save = false;
    switch (file.GetAction())
    {
        case File::SAVE_BATTERY: save = true; // fall through
        case File::LOAD_BATTERY: suffix = ".nestopia.battery"; break;
        case File::SAVE_EEPROM: save = true; // fall through
        case File::LOAD_EEPROM: suffix = ".nestopia.eeprom"; break;
        case File::SAVE_FDS: save = true; // fall through
        case File::LOAD_FDS: suffix = ".nestopia.ips"; break;
        default: return;
    }
    char *base = rg_emu_get_path(RG_PATH_SAVE_SRAM, app->romPath);
    std::string path = std::string(base) + suffix;
    free(base);
    if (save)
    {
        rg_storage_mkdir(rg_dirname(path.c_str()));
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        Nes::Result result = file.GetAction() == File::SAVE_FDS ?
            file.GetPatchContent(File::PATCH_IPS, output) : file.GetContent(output);
        output.flush();
        if (result < Nes::RESULT_OK || !output.good())
            RG_LOGE("Nestopia: could not save persistent data: %s", path.c_str());
    }
    else
    {
        std::ifstream input(path, std::ios::binary);
        if (input)
        {
            Nes::Result result = file.GetAction() == File::LOAD_FDS ?
                file.SetPatchContent(input) : file.SetContent(input);
            if (result < Nes::RESULT_OK)
                RG_LOGE("Nestopia: could not load persistent data: %s", path.c_str());
        }
    }
}

static void draw_nsf_overlay(void)
{
    Nes::Api::Nsf nsf(*activeEmulator);
    char song[32];
    snprintf(song, sizeof(song), "%d / %u", nsf.GetCurrentSong() + 1, nsf.GetNumSongs());

    const rg_gui_option_t options[] = {
        {0, _("Name"), (char *)nsf.GetName(), RG_DIALOG_FLAG_NORMAL, NULL},
        {0, _("Artist"), (char *)nsf.GetArtist(), RG_DIALOG_FLAG_NORMAL, NULL},
        {0, _("Copyright"), (char *)nsf.GetCopyright(), RG_DIALOG_FLAG_NORMAL, NULL},
        {0, _("Playing"), song, RG_DIALOG_FLAG_NORMAL, NULL},
        RG_DIALOG_END,
    };
    rg_gui_draw_dialog("NSF Player", options, 4, -1);
}

static void submit_video(void)
{
    // The display borrows this persistent surface. Release any previous borrow
    // before changing its crop metadata (also applies to menu redraw events).
    rg_display_sync(true);
    int cropV = overscan && refreshRate == 60 ? 8 : 0;
    int cropH = autocrop ? 8 : 0;
    currentUpdate->width = 256 - cropH * 2;
    currentUpdate->height = 240 - cropV * 2;
    currentUpdate->offset = cropV * currentUpdate->stride + cropH * 2;
    rg_display_submit(currentUpdate, RG_DISPLAY_SUBMIT_BORROW);
}

static void event_handler(int event, void *arg)
{
    (void)arg;
    if (event == RG_EVENT_SHUTDOWN && activeMachine)
    {
        // Power-off invokes Nestopia's battery, EEPROM and disk save callbacks.
        activeMachine->Power(false);
        return;
    }
    if (event != RG_EVENT_REDRAW)
        return;

    if (nsfPlayer)
    {
        rg_display_clear(C_BLACK);
        draw_nsf_overlay();
    }
    else if (currentUpdate)
    {
        submit_video();
    }
}

static bool screenshot_handler(const char *filename, int width, int height)
{
    return currentUpdate && rg_surface_save_image_file(currentUpdate, filename, width, height);
}

static bool save_state_handler(const char *filename)
{
    if (!activeMachine)
        return false;

    std::ofstream output(filename, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!output)
    {
        RG_LOGE("Nestopia: cannot open state file for writing: %s", filename);
        return false;
    }

    Nes::Result result = activeMachine->SaveState(output, Nes::Api::Machine::NO_COMPRESSION);
    output.flush();
    bool success = result >= Nes::RESULT_OK && output.good();
    output.close();
    if (!success)
        RG_LOGE("Nestopia: state save failed (result %d)", result);
    return success;
}

static bool load_state_handler(const char *filename)
{
    if (!activeMachine)
        return false;

    std::ifstream input(filename, std::ios::in | std::ios::binary);
    if (!input)
    {
        RG_LOGE("Nestopia: cannot open state file: %s", filename);
        return false;
    }

    Nes::Result result = activeMachine->LoadState(input);
    if (result < Nes::RESULT_OK)
    {
        RG_LOGE("Nestopia: state load failed (result %d)", result);
        if (activeMachine->Reset(true) < Nes::RESULT_OK)
            RG_LOGE("Nestopia: reset after state load failure also failed.");
        return false;
    }
    return true;
}

static bool reset_handler(bool hard)
{
    if (!activeMachine)
        return false;
    return activeMachine->Reset(hard) >= Nes::RESULT_OK;
}

static rg_gui_event_t sprite_limit_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    Nes::Api::Video video(*activeEmulator);
    bool spriteLimit = !video.AreUnlimSpritesEnabled();

    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        spriteLimit = !spriteLimit;
        Nes::Result result = video.EnableUnlimSprites(!spriteLimit);
        if (result < Nes::RESULT_OK)
            RG_LOGE("Nestopia: could not change sprite limit (result %d)", result);
        else
            rg_settings_set_number(NS_APP, SETTING_SPRITELIMIT, spriteLimit);
    }

    strcpy(option->value, spriteLimit ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t overscan_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        overscan = !overscan;
        rg_settings_set_number(NS_APP, SETTING_OVERSCAN, overscan);
        return RG_DIALOG_REDRAW;
    }

    strcpy(option->value, overscan ? _("Auto") : _("Off"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t autocrop_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    int value = autocrop;
    if (event == RG_DIALOG_PREV)
        value = value > 0 ? value - 1 : 2;
    if (event == RG_DIALOG_NEXT)
        value = value < 2 ? value + 1 : 0;

    if (value != autocrop)
    {
        autocrop = value;
        rg_settings_set_number(NS_APP, SETTING_AUTOCROP, value);
        return RG_DIALOG_REDRAW;
    }

    if (value == 0) strcpy(option->value, _("Never"));
    if (value == 1) strcpy(option->value, _("Auto"));
    if (value == 2) strcpy(option->value, _("Always"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t palette_update_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    static const char *const names[] = {
        "Canonical", "Consumer", "Alternative", "Sony CXA2025AS (JP)", "Sony CXA2025AS (US)",
    };
    static const Nes::Api::Video::DecoderPreset presets[] = {
        Nes::Api::Video::DECODER_CANONICAL,
        Nes::Api::Video::DECODER_CONSUMER,
        Nes::Api::Video::DECODER_ALTERNATIVE,
        Nes::Api::Video::DECODER_CXA2025AS_JP,
        Nes::Api::Video::DECODER_CXA2025AS_US,
    };
    const int max = sizeof(names) / sizeof(names[0]) - 1;
    int value = palette;

    if (event == RG_DIALOG_PREV)
        value = value > 0 ? value - 1 : max;
    if (event == RG_DIALOG_NEXT)
        value = value < max ? value + 1 : 0;

    if (value != palette)
    {
        Nes::Result result = activeVideo->SetDecoder(Nes::Api::Video::Decoder(presets[value]));
        if (result < Nes::RESULT_OK)
        {
            RG_LOGE("Nestopia: could not change color decoder (result %d)", result);
            return RG_DIALOG_VOID;
        }
        palette = value;
        rg_settings_set_number(NS_APP, SETTING_PALETTE, palette);
        return RG_DIALOG_REDRAW;
    }

    strcpy(option->value, _(names[value]));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t disk_side_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    Nes::Api::Fds fds(*activeEmulator);
    int side = fds.IsAnyDiskInserted() ? fds.GetCurrentDisk() * 2 + fds.GetCurrentDiskSide() : -1;
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        const int count = fds.GetNumSides();
        side = event == RG_DIALOG_NEXT ? (side + 1) % count : (side + count - 1) % count;
        if (fds.EjectDisk() < Nes::RESULT_OK || fds.InsertDisk(side / 2, side % 2) < Nes::RESULT_OK)
            RG_LOGE("Nestopia: could not change disk side.");
    }
    snprintf(option->value, 32, "%d %c", side / 2 + 1, 'A' + side % 2);
    return RG_DIALOG_VOID;
}

static void options_handler(rg_gui_option_t *dest)
{
    *dest++ = rg_gui_option_t{0, _("Palette"), "-", RG_DIALOG_FLAG_NORMAL, &palette_update_cb};
    *dest++ = rg_gui_option_t{0, _("Overscan"), "-", RG_DIALOG_FLAG_NORMAL, &overscan_update_cb};
    *dest++ = rg_gui_option_t{0, _("Crop sides"), "-", RG_DIALOG_FLAG_NORMAL, &autocrop_update_cb};
    *dest++ = rg_gui_option_t{0, _("Sprite limit"), "-", RG_DIALOG_FLAG_NORMAL, &sprite_limit_cb};
    if (activeMachine->Is(Nes::Api::Machine::DISK))
        *dest++ = rg_gui_option_t{0, _("Disk side"), "-", RG_DIALOG_FLAG_NORMAL, &disk_side_cb};
    *dest++ = rg_gui_option_t RG_DIALOG_END;
}

static uint32_t convert_buttons(uint32_t joystick)
{
    uint32_t buttons = 0;
    if (joystick & RG_KEY_A) buttons |= Nes::Core::Input::Controllers::Pad::A;
    if (joystick & RG_KEY_B) buttons |= Nes::Core::Input::Controllers::Pad::B;
    if (joystick & RG_KEY_SELECT) buttons |= Nes::Core::Input::Controllers::Pad::SELECT;
    if (joystick & RG_KEY_START) buttons |= Nes::Core::Input::Controllers::Pad::START;
    if (joystick & RG_KEY_UP) buttons |= Nes::Core::Input::Controllers::Pad::UP;
    if (joystick & RG_KEY_DOWN) buttons |= Nes::Core::Input::Controllers::Pad::DOWN;
    if (joystick & RG_KEY_LEFT) buttons |= Nes::Core::Input::Controllers::Pad::LEFT;
    if (joystick & RG_KEY_RIGHT) buttons |= Nes::Core::Input::Controllers::Pad::RIGHT;
    return buttons;
}

static Nes::Result load_image(Nes::Api::Machine &machine, const char *path)
{
    if (rg_extension_match(path, "zip"))
    {
        void *data = NULL;
        size_t size = 0;
        if (!rg_storage_unzip_file(path, NULL, &data, &size, RG_FILE_ALIGN_8KB))
            RG_PANIC("Nestopia: ROM file unzipping failed.");
        if (!data || !size)
        {
            free(data);
            RG_PANIC("Nestopia: ZIP archive contains no ROM data.");
        }

        MemoryStreamBuffer buffer(data, size);
        std::istream input(&buffer);
        Nes::Result result = machine.Load(input, Nes::Api::Machine::FAVORED_NES_NTSC);
        free(data);
        return result;
    }

    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input)
    {
        RG_LOGE("Nestopia: cannot open ROM: %s", path);
        return Nes::RESULT_ERR_INVALID_FILE;
    }
    return machine.Load(input, Nes::Api::Machine::FAVORED_NES_NTSC);
}

static void handle_nsf_input(Nes::Api::Nsf &nsf, uint32_t joystick)
{
    const uint32_t pressed = joystick & ~previousNsfButtons;
    Nes::Result result = Nes::RESULT_NOP;
    if (pressed & RG_KEY_A)
        result = nsf.PlaySong();
    else if (pressed & RG_KEY_B)
        result = nsf.StopSong();
    else if (pressed & RG_KEY_LEFT)
        result = nsf.SelectPrevSong();
    else if (pressed & RG_KEY_RIGHT)
        result = nsf.SelectNextSong();
    if (result < Nes::RESULT_OK)
        RG_LOGE("Nestopia: NSF transport action failed (result %d)", result);
    previousNsfButtons = joystick;
}

void nes_nestopia_main(void)
{
    rg_handlers_t handlers = {};
    handlers.loadState = &load_state_handler;
    handlers.saveState = &save_state_handler;
    handlers.reset = &reset_handler;
    handlers.event = &event_handler;
    handlers.screenshot = &screenshot_handler;
    handlers.options = &options_handler;

    app = rg_system_reinit(AUDIO_SAMPLE_RATE, &handlers, NULL);
    overscan = rg_settings_get_number(NS_APP, SETTING_OVERSCAN, 1);
    autocrop = rg_settings_get_number(NS_APP, SETTING_AUTOCROP, 0);
    palette = rg_settings_get_number(NS_APP, SETTING_PALETTE, 0);
    if (palette < 0 || palette > 4)
        palette = 0;

    try
    {
        activeEmulator = new Nes::Api::Emulator();
    }
    catch (const std::bad_alloc &)
    {
        RG_PANIC("Nestopia: emulator allocation failed.");
    }
    // Reserve the hot machine and PPU screen before RGB surfaces fragment the
    // internal heap. RGB conversion runs only for presentations; the PPU screen
    // is read/written every emulated frame and benefits much more from SRAM.
    updates[0] = rg_surface_create(256, 240, RG_PIXEL_565_LE, MEM_FAST);
    // Borrow the persistent source until the asynchronous display releases it.
    // One source surface suffices, saving 120 KiB of scarce internal SRAM.
    updates[1] = rg_display_uses_frame_copy() ? updates[0] :
        rg_surface_create(256, 240, RG_PIXEL_565_LE, MEM_FAST);
    if (!updates[0] || !updates[1])
        RG_PANIC("Nestopia: video buffer allocation failed.");
    currentUpdate = updates[0];
#ifdef RG_NESTOPIA_PROFILE
    Nes::Core::Machine &core = *activeEmulator;
    printf("NES-MEM machine=%p bytes=%u cpu=%u ppu=%u\n", (void *)&core,
           (unsigned)sizeof(core), (unsigned)sizeof(core.cpu), (unsigned)sizeof(core.ppu));
    printf("NES-ROM %s pixels=%p\n", app->romPath, (void *)core.ppu.GetScreen().pixels);
#endif

    Nes::Api::Machine machine(*activeEmulator);
    Nes::Api::Fds fds(*activeEmulator);
    Nes::Api::Video video(*activeEmulator);
    activeMachine = &machine;
    activeVideo = &video;
    Nes::Api::User::fileIoCallback.Set(file_io, NULL);

    bool spriteLimit = rg_settings_get_number(NS_APP, SETTING_SPRITELIMIT, 1) != 0;
    if (video.EnableUnlimSprites(!spriteLimit) < Nes::RESULT_OK)
        RG_PANIC("Nestopia: could not configure sprite limit.");

    std::ifstream bios(nes_fds_bios_path(), std::ios::in | std::ios::binary);
    if (bios)
    {
        Nes::Result result = fds.SetBIOS(&bios);
        if (result < Nes::RESULT_OK)
        {
            RG_LOGE("Nestopia: FDS BIOS load failed (result %d)", result);
            RG_PANIC("Nestopia: FDS BIOS load failed.");
        }
    }

    Nes::Result result = load_image(machine, app->romPath);
    if (result < Nes::RESULT_OK)
    {
        RG_LOGE("Nestopia: ROM load failed (result %d)", result);
        RG_PANIC("Nestopia: ROM load failed.");
    }
    if (machine.SetMode(machine.GetDesiredMode()) < Nes::RESULT_OK)
        RG_PANIC("Nestopia: region configuration failed.");
#ifdef RG_NESTOPIA_PROFILE
    if (const auto *profile = Nes::Api::Cartridge(*activeEmulator).GetProfile())
        printf("NES-MAPPER id=%u submapper=%u\n", unsigned(profile->board.mapper), unsigned(profile->board.subMapper));
#endif

    Nes::Api::Video::RenderState renderState;
    renderState.width = 256;
    renderState.height = 240;
    renderState.bits.count = 16;
    renderState.bits.mask.r = 0xF800;
    renderState.bits.mask.g = 0x07E0;
    renderState.bits.mask.b = 0x001F;
    renderState.filter = Nes::Api::Video::RenderState::FILTER_NONE;
    if (video.SetRenderState(renderState) < Nes::RESULT_OK)
        RG_PANIC("Nestopia: video output configuration failed.");

    Nes::Api::Video::Palette nesPalette = video.GetPalette();
    if (nesPalette.SetMode(Nes::Api::Video::Palette::MODE_YUV) < Nes::RESULT_OK ||
        video.SetDecoder(Nes::Api::Video::Decoder(
            static_cast<Nes::Api::Video::DecoderPreset>(palette))) < Nes::RESULT_OK)
        RG_PANIC("Nestopia: palette configuration failed.");

    Nes::Api::Input(*activeEmulator).AutoSelectControllers();
    Nes::Api::Sound sound(*activeEmulator);
    if (sound.SetSampleRate(app->sampleRate) < Nes::RESULT_OK)
        RG_PANIC("Nestopia: audio sample rate configuration failed.");

    if (machine.Power(true) < Nes::RESULT_OK)
        RG_PANIC("Nestopia: machine power-on failed.");
    if (machine.Is(Nes::Api::Machine::DISK) && fds.InsertDisk(0, 0) < Nes::RESULT_OK)
        RG_PANIC("Nestopia: disk insertion failed.");

    nsfPlayer = machine.Is(Nes::Api::Machine::SOUND);
    Nes::Api::Nsf nsf(*activeEmulator);
    previousNsfButtons = 0;
    if (nsfPlayer && nsf.PlaySong() < Nes::RESULT_OK)
        RG_PANIC("Nestopia: NSF playback could not start.");

    refreshRate = machine.GetMode() == Nes::Api::Machine::PAL ? 50 : 60;
    rg_system_apply_saved_overclock();
    rg_system_set_tick_rate(refreshRate);
    app->frameskip = rg_display_uses_frame_copy() ? 0 : 1;
    // Nestopia simulates every PPU dot even without RGB output. Raising Auto
    // skip to 5 barely saves emulation work and needlessly reduces video to
    // one frame in six. Let the owned display queue decide presentations.
    app->frameskipDisplayOnly = rg_display_uses_frame_copy();

    if (app->bootFlags & RG_BOOT_RESUME)
        rg_emu_load_state(app->saveSlot);

    int skipFrames = 0;
    unsigned nsfOverlayFrames = 0;
#ifdef RG_NESTOPIA_PROFILE
    int64_t profileStart = rg_system_timer(), executeTime = 0, audioTime = 0, videoTime = 0;
    unsigned profileFrames = 0, drawnFrames = 0;
    int profileDisplayStart = rg_display_get_counters().totalFrames;
#endif
    while (true)
    {
        uint32_t joystick = rg_input_read_gamepad();
        if (joystick & (RG_KEY_MENU | RG_KEY_OPTION))
        {
            if (joystick & RG_KEY_MENU)
                rg_gui_game_menu();
            else
                rg_gui_options_menu();
        }

        if (nsfPlayer)
            handle_nsf_input(nsf, joystick);

        int64_t startTime = rg_system_timer();
        const bool wantFrame = !skipFrames && !nsfPlayer;

        Nes::Core::Input::Controllers input;
        if (!nsfPlayer)
        {
            input.pad[0].buttons = convert_buttons(joystick);
            input.pad[1].buttons = convert_buttons(rg_input_read_gamepad_player(RG_PLAYER_2));
        }

        // Drain every sample generated by a frame, including fractional carry.
        const unsigned requestedSamples = AUDIO_BUFFER_LENGTH;
        Nes::Core::Sound::Output audioOutput(audioMono, requestedSamples);
        result = activeEmulator->Execute(NULL, &audioOutput, &input);
#ifdef RG_NESTOPIA_PROFILE
        int64_t executeEnd = rg_system_timer();
        executeTime += executeEnd - startTime;
#endif
        if (result < Nes::RESULT_OK)
        {
            RG_LOGE("Nestopia: frame execution failed (result %d)", result);
            RG_PANIC("Nestopia: emulation stopped.");
        }

        unsigned samples = audioOutput.length[0];
        if (samples > AUDIO_BUFFER_LENGTH)
            RG_PANIC("Nestopia: audio output exceeded its buffer.");
        if (nsfPlayer && !nsf.IsPlaying())
            memset(audioMono, 0, samples * sizeof(audioMono[0]));
        for (unsigned i = 0; i < samples; i++)
        {
            audioStereo[i].left = audioMono[i];
            audioStereo[i].right = audioMono[i];
        }
        if (samples)
            rg_audio_submit(audioStereo, samples);
#ifdef RG_NESTOPIA_PROFILE
        int64_t audioEnd = rg_system_timer();
        audioTime += audioEnd - executeEnd;
#endif

        // Decide after emulation/audio: the display may have become available
        // during this frame. Blit is the public API for the completed PPU screen.
        // Keeping this single surface unchanged until release avoids an extra
        // full RGB frame copy and never races the display task's reads.
        const bool drawFrame = wantFrame &&
            (!rg_display_uses_frame_copy() || rg_display_sync(false));
        if (drawFrame)
        {
            currentUpdate = updates[currentUpdate == updates[0] ? 1 : 0];
            Nes::Core::Video::Output videoOutput(currentUpdate->data, currentUpdate->stride);
            if (video.Blit(videoOutput) < Nes::RESULT_OK)
                RG_PANIC("Nestopia: video conversion failed.");
            submit_video();
        }
        if (nsfPlayer && (++nsfOverlayFrames % 10 == 0))
            draw_nsf_overlay();
#ifdef RG_NESTOPIA_PROFILE
        videoTime += rg_system_timer() - audioEnd;
        drawnFrames += drawFrame;
        if (++profileFrames == 300)
        {
            int displayed = rg_display_get_counters().totalFrames;
            double seconds = (rg_system_timer() - profileStart) / 1000000.0;
            printf("NES-PERF frames=%u fps=%.2f rendered=%.2f presented=%.2f execute_us=%lld audio_us=%lld video_us=%lld\n",
                   profileFrames, profileFrames / seconds, drawnFrames / seconds,
                   (displayed - profileDisplayStart) / seconds,
                   (long long)(executeTime / profileFrames), (long long)(audioTime / profileFrames),
                   (long long)(videoTime / profileFrames));
            printf("NES-CORE ppu_cycles=%llu apu_walk_cycles=%llu apu_sample_cycles=%llu tiles=%u\n",
                   (unsigned long long)(RetroGo::ppuCycles / profileFrames),
                   (unsigned long long)(RetroGo::apuWalkCycles / profileFrames),
                   (unsigned long long)(RetroGo::apuSampleCycles / profileFrames),
                   unsigned(RetroGo::tileBatches / profileFrames));
            RetroGo::ppuCycles = RetroGo::apuWalkCycles = RetroGo::apuSampleCycles = 0;
            RetroGo::tileBatches = 0;
            profileStart = rg_system_timer();
            profileDisplayStart = displayed;
            profileFrames = drawnFrames = 0;
            executeTime = audioTime = videoTime = 0;
        }
#endif

        rg_system_tick(rg_system_timer() - startTime);

        if (skipFrames == 0)
        {
            int frameTime = 1000000 / (refreshRate * app->speed);
            int elapsed = rg_system_timer() - startTime;
            skipFrames = rg_system_get_next_frameskip(elapsed > frameTime + 1500);
        }
        else
        {
            skipFrames--;
        }
    }
}
