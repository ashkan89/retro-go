#include "legacy.h"
#include "Console.hxx"
#include "Cart.hxx"
#include "MD5.hxx"
#include "OSystem.hxx"
#include "Settings.hxx"
#include "PropsSet.hxx"
#include "EventHandler.hxx"
#include "Switches.hxx"
#include "SoundSDL.hxx"
#include "Serializer.hxx"
#include "System.hxx"
#include "TIA.hxx"

static OSystem *osystem;
static Console *console;
static uint16_t palette[256];
static rg_audio_sample_t audio[640];
static unsigned samples;
static unsigned sample_remainder;
static string digest;

static void init(const char *path)
{
    size_t size;
    uInt8 *data = (uInt8 *)legacy_read_file(path, &size, 512 * 1024);
    RG_ASSERT(data, "Unable to load Atari 2600 ROM");
    digest = MD5(data, size);
    osystem = new OSystem();
    Settings *settings = new Settings(osystem);
    settings->setValue("romloadcount", false);
    settings->setValue("sound", true);
    settings->setValue("volume", (Int32)100);
    Properties props;
    osystem->propSet().getMD5(digest, props);
    string type = props.get(Cartridge_Type), id;
    Cartridge *cart = Cartridge::create(data, size, digest, type, id, *osystem, *settings);
    free(data);
    RG_ASSERT(cart, "Unsupported Atari 2600 cartridge");
    console = new Console(osystem, cart, props);
    osystem->myConsole = console;
    console->initializeVideo();
    console->initializeAudio();
    stella_core.width = console->tia().width();
    stella_core.height = console->tia().height();
    stella_core.refresh_rate = (int)(console->getFramerate() + 0.5f);
    sample_remainder = 0;
    samples = (31400 + stella_core.refresh_rate - 1) / stella_core.refresh_rate;
    RG_ASSERT(samples <= RG_COUNT(audio), "Unsupported Atari 2600 frame rate");
    const uInt32 *colors = console->getPalette(0);
    for (int i = 0; i < 256; i++)
        palette[i] = ((colors[i] >> 8) & 0xF800) | ((colors[i] >> 5) & 0x07E0) | ((colors[i] >> 3) & 0x1F);
}

static void step(uint32_t keys, rg_surface_t *surface)
{
    Event &event = osystem->eventHandler().event();
    event.set(Event::JoystickZeroUp, !!(keys & RG_KEY_UP));
    event.set(Event::JoystickZeroDown, !!(keys & RG_KEY_DOWN));
    event.set(Event::JoystickZeroLeft, !!(keys & RG_KEY_LEFT));
    event.set(Event::JoystickZeroRight, !!(keys & RG_KEY_RIGHT));
    event.set(Event::JoystickZeroFire, !!(keys & (RG_KEY_A | RG_KEY_B)));
    event.set(Event::ConsoleSelect, !!(keys & RG_KEY_SELECT));
    event.set(Event::ConsoleReset, !!(keys & RG_KEY_START));
    console->controller(Controller::Left).update();
    console->controller(Controller::Right).update();
    console->switches().update();
    console->tia().update();
    sample_remainder += 31400;
    samples = sample_remainder / stella_core.refresh_rate;
    sample_remainder %= stella_core.refresh_rate;
    ((SoundSDL *)&osystem->sound())->processFragment((Int16 *)audio, samples);
    rg_audio_submit(audio, samples);
    if (RenderFlag) {
        const uInt8 *pixels = console->tia().currentFrameBuffer();
        uint16_t *dest = (uint16_t *)surface->data;
        for (int i = 0; i < surface->width * surface->height; i++) dest[i] = palette[pixels[i]];
    }
}

static bool save(const char *path)
{
    try {
        Serializer state(path);
        if (!state.isValid()) return false;
        state.putString(digest);
        return console->save(state) && state.flush();
    } catch (...) { return false; }
}

static bool load(const char *path)
{
    try {
        Serializer state(path, true);
        if (!state.isValid()) return false;
        if (state.getString() != digest) return false;
        bool ok = console->load(state);
        ((SoundSDL *)&osystem->sound())->reset();
        sample_remainder = 0;
        return ok;
    } catch (...) { return false; }
}

static bool reset(bool hard)
{
    console->system().reset();
    ((SoundSDL *)&osystem->sound())->reset();
    sample_remainder = 0;
    return true;
}

legacy_core_t stella_core = {init, step, save, load, reset, NULL, 160, 210, 31400, 60};
