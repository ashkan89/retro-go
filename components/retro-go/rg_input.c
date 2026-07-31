#include "rg_system.h"
#include "rg_input.h"
#include "rg_usb_hid.h"
#include "rg_usb_xinput.h"
#include "rg_usb_msc.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef ESP_PLATFORM
#include <driver/gpio.h>
#include <driver/adc.h>
// This is a lazy way to silence deprecation notices on some esp-idf versions...
// This hardcoded value is the first thing to check if something stops working!
#define ADC_ATTEN_DB_11 3
#else
#include <SDL2/SDL.h>
#endif

#if RG_BATTERY_DRIVER == 1
#include <esp_adc_cal.h>
static esp_adc_cal_characteristics_t adc_chars;
#endif

#ifdef RG_GAMEPAD_ADC_MAP
static rg_keymap_adc_t keymap_adc[] = RG_GAMEPAD_ADC_MAP;
#endif
#ifdef RG_GAMEPAD_GPIO_MAP
static rg_keymap_gpio_t keymap_gpio[] = RG_GAMEPAD_GPIO_MAP;
#endif
#ifdef RG_GAMEPAD_I2C_MAP
static rg_keymap_i2c_t keymap_i2c[] = RG_GAMEPAD_I2C_MAP;
#endif
#ifdef RG_GAMEPAD_KBD_MAP
static rg_keymap_kbd_t keymap_kbd[] = RG_GAMEPAD_KBD_MAP;
#endif
#ifdef RG_GAMEPAD_SERIAL_MAP
static rg_keymap_serial_t keymap_serial[] = RG_GAMEPAD_SERIAL_MAP;
#endif
#ifdef RG_GAMEPAD_VIRT_MAP
static rg_keymap_virt_t keymap_virt[] = RG_GAMEPAD_VIRT_MAP;
#endif
static bool input_task_running = false;
static uint32_t gamepad_state[RG_PLAYER_COUNT] = {(uint32_t)-1, (uint32_t)-1}; // _Atomic
static uint32_t gamepad_mapped = 0;
static rg_battery_t battery_state = {0};

// Local multiplayer: per-source player assignment (RG_INPUT_PLAYER_AUTO/OFF or RG_PLAYER_1/2)
// and the currently resolved player for each source (RG_INPUT_PLAYER_OFF or RG_PLAYER_1/2).
static int8_t source_assignment[RG_INPUT_SOURCE_COUNT];
static int8_t source_resolved_player[RG_INPUT_SOURCE_COUNT];
static uint16_t source_connect_seq[RG_INPUT_SOURCE_COUNT];
static bool source_was_connected[RG_INPUT_SOURCE_COUNT];
static uint16_t connect_seq_counter;

#define UPDATE_GLOBAL_MAP(keymap)                 \
    for (size_t i = 0; i < RG_COUNT(keymap); ++i) \
        gamepad_mapped |= keymap[i].key;          \

#ifdef ESP_PLATFORM
static inline int adc_get_raw(adc_unit_t unit, adc_channel_t channel)
{
    if (unit == ADC_UNIT_1)
    {
        return adc1_get_raw(channel);
    }
    else if (unit == ADC_UNIT_2)
    {
        int adc_raw_value = -1;
        if (adc2_get_raw(channel, ADC_WIDTH_MAX - 1, &adc_raw_value) != ESP_OK)
            RG_LOGE("ADC2 reading failed, this can happen while wifi is active.");
        return adc_raw_value;
    }
    RG_LOGE("Invalid ADC unit %d", (int)unit);
    return -1;
}
#endif

bool rg_input_read_battery_raw(rg_battery_t *out)
{
    uint32_t raw_value = 0;
    bool present = true;
    bool charging = false;

#if RG_BATTERY_DRIVER == 1 /* ADC */
    for (int i = 0; i < 4; ++i)
    {
        int value = adc_get_raw(RG_BATTERY_ADC_UNIT, RG_BATTERY_ADC_CHANNEL);
        if (value < 0)
            return false;
        raw_value += esp_adc_cal_raw_to_voltage(value, &adc_chars);
    }
    raw_value /= 4;
#elif RG_BATTERY_DRIVER == 2 /* I2C */
    uint8_t data[5];
    if (!rg_i2c_read(0x20, -1, &data, 5))
        return false;
    raw_value = data[4];
    charging = data[4] == 255;
#else
    return false;
#endif

    if (!out)
        return true;

    (void)raw_value;

    *out = (rg_battery_t){
        .level = RG_MAX(0.f, RG_MIN(100.f, RG_BATTERY_CALC_PERCENT(raw_value))),
        .volts = RG_BATTERY_CALC_VOLTAGE(raw_value),
        .present = present,
        .charging = charging,
    };
    return true;
}

static uint32_t apply_virt_map(uint32_t state)
{
#if defined(RG_GAMEPAD_VIRT_MAP)
    for (size_t i = 0; i < RG_COUNT(keymap_virt); ++i)
    {
        if (state == keymap_virt[i].src)
            return keymap_virt[i].key;
    }
#endif
    return state;
}

// Combines every built-in (non-USB) input driver into a single bitmask. There is only ever
// one built-in control surface, so unlike USB sources it doesn't need a player slot of its own.
static uint32_t read_builtin_state(void)
{
    uint32_t state = 0;

#if defined(RG_GAMEPAD_ADC_MAP)
    static int old_adc_values[RG_COUNT(keymap_adc)];
    for (size_t i = 0; i < RG_COUNT(keymap_adc); ++i)
    {
        const rg_keymap_adc_t *mapping = &keymap_adc[i];
        int value = adc_get_raw(mapping->unit, mapping->channel);
        if (value >= mapping->min && value <= mapping->max)
        {
            if (abs(old_adc_values[i] - value) < RG_GAMEPAD_ADC_FILTER_WINDOW)
                state |= mapping->key;
            // else
            //     RG_LOGD("Rejected input: %d", old_adc_values[i] - value);
            old_adc_values[i] = value;
        }
    }
#endif

#if defined(RG_GAMEPAD_GPIO_MAP)
    for (size_t i = 0; i < RG_COUNT(keymap_gpio); ++i)
    {
        const rg_keymap_gpio_t *mapping = &keymap_gpio[i];
        if (gpio_get_level(mapping->num) == mapping->level)
            state |= mapping->key;
    }
#endif

#if defined(RG_GAMEPAD_I2C_MAP)
    uint32_t buttons = 0;
#if defined(RG_I2C_GPIO_DRIVER)
    int data0 = rg_i2c_gpio_read_port(0), data1 = rg_i2c_gpio_read_port(1);
    if (data0 > -1) // && data1 > -1)
    {
        buttons = (data1 << 8) | (data0);
#elif defined(RG_TARGET_T_DECK_PLUS)
    uint8_t data[5];
    if (rg_i2c_read(T_DECK_KBD_ADDRESS, -1, &data, 5))
    {
        buttons = ((data[0] << 25) | (data[1] << 18) | (data[2] << 11) | ((data[3] & 0xF8) << 4) | (data[4]));
#else
    uint8_t data[5];
    if (rg_i2c_read(RG_I2C_GPIO_ADDR, -1, &data, 5))
    {
        buttons = (data[2] << 8) | (data[1]);
#endif
        for (size_t i = 0; i < RG_COUNT(keymap_i2c); ++i)
        {
            const rg_keymap_i2c_t *mapping = &keymap_i2c[i];
            if (((buttons >> mapping->num) & 1) == mapping->level)
                state |= mapping->key;
        }
    }
#endif

#if defined(RG_GAMEPAD_KBD_MAP)
#ifdef RG_TARGET_SDL2
    int numkeys = 0;
    const uint8_t *keys = SDL_GetKeyboardState(&numkeys);
    for (size_t i = 0; i < RG_COUNT(keymap_kbd); ++i)
    {
        const rg_keymap_kbd_t *mapping = &keymap_kbd[i];
        if (mapping->src < 0 || mapping->src >= numkeys)
            continue;
        if (keys[mapping->src])
            state |= mapping->key;
    }
#else
#warning "not implemented"
#endif
#endif

#if defined(RG_GAMEPAD_SERIAL_MAP)
    gpio_set_level(RG_GPIO_GAMEPAD_LATCH, 0);
    rg_usleep(5);
    gpio_set_level(RG_GPIO_GAMEPAD_LATCH, 1);
    rg_usleep(1);
    uint32_t buttons = 0;
    for (int i = 0; i < 16; i++)
    {
        buttons |= gpio_get_level(RG_GPIO_GAMEPAD_DATA) << (15 - i);
        gpio_set_level(RG_GPIO_GAMEPAD_CLOCK, 0);
        rg_usleep(1);
        gpio_set_level(RG_GPIO_GAMEPAD_CLOCK, 1);
        rg_usleep(1);
    }
    for (size_t i = 0; i < RG_COUNT(keymap_serial); ++i)
    {
        const rg_keymap_serial_t *mapping = &keymap_serial[i];
        if (((buttons >> mapping->num) & 1) == mapping->level)
            state |= mapping->key;
    }
#endif

    return state;
}

static uint32_t read_source_state(rg_input_source_t source)
{
    switch (source)
    {
    case RG_INPUT_SOURCE_BUILTIN: return read_builtin_state();
    case RG_INPUT_SOURCE_USB_GAMEPAD_1: return rg_usb_hid_get_gamepad_state(0);
    case RG_INPUT_SOURCE_USB_GAMEPAD_2: return rg_usb_hid_get_gamepad_state(1);
    case RG_INPUT_SOURCE_USB_KEYBOARD: return rg_usb_hid_get_keyboard_state();
    case RG_INPUT_SOURCE_USB_MOUSE: return rg_usb_hid_get_mouse_state();
    case RG_INPUT_SOURCE_XINPUT_1: return rg_usb_xinput_get_gamepad_state(0);
    case RG_INPUT_SOURCE_XINPUT_2: return rg_usb_xinput_get_gamepad_state(1);
    default: return 0;
    }
}

static bool source_connected(rg_input_source_t source)
{
    switch (source)
    {
    case RG_INPUT_SOURCE_BUILTIN: return true;
    case RG_INPUT_SOURCE_USB_GAMEPAD_1: return rg_usb_hid_get_gamepad_connected(0);
    case RG_INPUT_SOURCE_USB_GAMEPAD_2: return rg_usb_hid_get_gamepad_connected(1);
    case RG_INPUT_SOURCE_USB_KEYBOARD: return (rg_usb_hid_get_connected() & (1U << RG_USB_HID_KEYBOARD)) != 0;
    case RG_INPUT_SOURCE_USB_MOUSE: return (rg_usb_hid_get_connected() & (1U << RG_USB_HID_MOUSE)) != 0;
    case RG_INPUT_SOURCE_XINPUT_1: return rg_usb_xinput_get_connected(0);
    case RG_INPUT_SOURCE_XINPUT_2: return rg_usb_xinput_get_connected(1);
    default: return false;
    }
}

static void load_player_assignments(void)
{
    char key[24];
    for (int s = 0; s < RG_INPUT_SOURCE_COUNT; ++s)
    {
        int default_assignment = (s == RG_INPUT_SOURCE_BUILTIN) ? RG_PLAYER_1 : RG_INPUT_PLAYER_AUTO;
        snprintf(key, sizeof(key), "InputPlayer_%d", s);
        source_assignment[s] = (int8_t)rg_settings_get_number(NS_GLOBAL, key, default_assignment);
    }
}

// Recomputes, for every source, which player (if any) it currently contributes to. Sources set
// to RG_INPUT_PLAYER_AUTO resolve to: builtin -> player 1, and the first-connected (by connect
// order) other auto source -> player 2. Explicit assignments always win.
static void update_source_resolution(void)
{
    bool connected[RG_INPUT_SOURCE_COUNT];

    for (int s = 0; s < RG_INPUT_SOURCE_COUNT; ++s)
    {
        connected[s] = source_connected((rg_input_source_t)s);
        if (connected[s] && !source_was_connected[s])
            source_connect_seq[s] = ++connect_seq_counter;
        else if (!connected[s])
            source_connect_seq[s] = 0;
        source_was_connected[s] = connected[s];
    }

    int auto_p2 = -1;
    for (int s = 1; s < RG_INPUT_SOURCE_COUNT; ++s) // Skip RG_INPUT_SOURCE_BUILTIN
    {
        if (connected[s] && source_assignment[s] == RG_INPUT_PLAYER_AUTO)
        {
            if (auto_p2 < 0 || source_connect_seq[s] < source_connect_seq[auto_p2])
                auto_p2 = s;
        }
    }

    for (int s = 0; s < RG_INPUT_SOURCE_COUNT; ++s)
    {
        int assignment = source_assignment[s];
        if (assignment == RG_INPUT_PLAYER_AUTO)
            source_resolved_player[s] = (s == RG_INPUT_SOURCE_BUILTIN) ? RG_PLAYER_1 :
                                         (s == auto_p2) ? RG_PLAYER_2 : RG_INPUT_PLAYER_OFF;
        else
            source_resolved_player[s] = assignment;
    }
}

bool rg_input_read_gamepad_raw(uint32_t *out)
{
    uint32_t state = read_builtin_state();

    for (int s = RG_INPUT_SOURCE_USB_GAMEPAD_1; s < RG_INPUT_SOURCE_COUNT; ++s)
        state |= read_source_state((rg_input_source_t)s);

    state = apply_virt_map(state);

    if (out)
        *out = state;
    return true;
}

static void input_task(void *arg)
{
    uint8_t debounce[RG_PLAYER_COUNT][RG_KEY_COUNT];
    uint32_t local_gamepad_state[RG_PLAYER_COUNT] = {0};
    uint32_t old_gamepad_state[RG_PLAYER_COUNT] = {0};
    int64_t next_battery_update = 0;
    bool feedback_ready = false;

    // Start the task with debounce history full to allow a button held during boot to be detected
    memset(debounce, 0xFF, sizeof(debounce));
    input_task_running = true;

    while (input_task_running)
    {
        update_source_resolution();

        uint32_t source_state[RG_INPUT_SOURCE_COUNT];
        for (int s = 0; s < RG_INPUT_SOURCE_COUNT; ++s)
            source_state[s] = read_source_state((rg_input_source_t)s);

        uint32_t player_raw[RG_PLAYER_COUNT] = {0};
        for (int s = 0; s < RG_INPUT_SOURCE_COUNT; ++s)
        {
            int player = source_resolved_player[s];
            if (player >= 0 && player < RG_PLAYER_COUNT)
                player_raw[player] |= source_state[s];
        }

        for (int player = 0; player < RG_PLAYER_COUNT; ++player)
        {
            uint32_t state = apply_virt_map(player_raw[player]);

            for (int i = 0; i < RG_KEY_COUNT; ++i)
            {
                uint32_t val = ((debounce[player][i] << 1) | ((state >> i) & 1));
                debounce[player][i] = val & 0xFF;

                if ((val & ((1 << RG_GAMEPAD_DEBOUNCE_PRESS) - 1)) == ((1 << RG_GAMEPAD_DEBOUNCE_PRESS) - 1))
                {
                    local_gamepad_state[player] |= (1 << i); // Pressed
                }
                else if ((val & ((1 << RG_GAMEPAD_DEBOUNCE_RELEASE) - 1)) == 0)
                {
                    local_gamepad_state[player] &= ~(1 << i); // Released
                }
            }
            gamepad_state[player] = local_gamepad_state[player];

#if RG_HAPTIC_INPUT_FEEDBACK_MS > 0
            uint32_t pressed = local_gamepad_state[player] & ~old_gamepad_state[player];
            if (feedback_ready && pressed)
                rg_system_vibrate(RG_HAPTIC_INPUT_FEEDBACK_MS);
            old_gamepad_state[player] = local_gamepad_state[player];
#endif
        }
        feedback_ready = true;

        if (rg_system_timer() >= next_battery_update)
        {
            rg_battery_t temp = {0};
            if (rg_input_read_battery_raw(&temp))
            {
                if (fabsf(battery_state.level - temp.level) < RG_BATTERY_UPDATE_THRESHOLD)
                    temp.level = battery_state.level;
                if (fabsf(battery_state.volts - temp.volts) < RG_BATTERY_UPDATE_THRESHOLD_VOLT)
                    temp.volts = battery_state.volts;
            }
            battery_state = temp;
            next_battery_update = rg_system_timer() + 2 * 1000000; // update every 2 seconds
        }

        rg_task_delay(10);
    }

    input_task_running = false;
    for (int player = 0; player < RG_PLAYER_COUNT; ++player)
        gamepad_state[player] = -1;
}

void rg_input_init(void)
{
    RG_ASSERT(!input_task_running, "Input already initialized!");

#if defined(RG_GAMEPAD_ADC_MAP)
    RG_LOGI("Initializing ADC gamepad driver...");
    adc1_config_width(ADC_WIDTH_MAX - 1);
    for (size_t i = 0; i < RG_COUNT(keymap_adc); ++i)
    {
        const rg_keymap_adc_t *mapping = &keymap_adc[i];
        if (mapping->unit == ADC_UNIT_1)
            adc1_config_channel_atten(mapping->channel, mapping->atten);
        else if (mapping->unit == ADC_UNIT_2)
            adc2_config_channel_atten(mapping->channel, mapping->atten);
        else
            RG_LOGE("Invalid ADC unit %d!", (int)mapping->unit);
    }
    UPDATE_GLOBAL_MAP(keymap_adc);
#endif

#if defined(RG_GAMEPAD_GPIO_MAP)
    RG_LOGI("Initializing GPIO gamepad driver...");
    for (size_t i = 0; i < RG_COUNT(keymap_gpio); ++i)
    {
        const rg_keymap_gpio_t *mapping = &keymap_gpio[i];
        gpio_set_direction(mapping->num, GPIO_MODE_INPUT);
        if (mapping->pullup && mapping->pulldown)
            gpio_set_pull_mode(mapping->num, GPIO_PULLUP_PULLDOWN);
        else if (mapping->pullup || mapping->pulldown)
            gpio_set_pull_mode(mapping->num, mapping->pullup ? GPIO_PULLUP_ONLY : GPIO_PULLDOWN_ONLY);
        else
            gpio_set_pull_mode(mapping->num, GPIO_FLOATING);
    }
    UPDATE_GLOBAL_MAP(keymap_gpio);
#endif

#if defined(RG_GAMEPAD_I2C_MAP)
    RG_LOGI("Initializing I2C gamepad driver...");
    rg_i2c_init();
#if defined(RG_I2C_GPIO_DRIVER)
    for (size_t i = 0; i < RG_COUNT(keymap_i2c); ++i)
    {
        const rg_keymap_i2c_t *mapping = &keymap_i2c[i];
        if (mapping->pullup)
            rg_i2c_gpio_set_direction(mapping->num, RG_GPIO_INPUT_PULLUP);
        else
            rg_i2c_gpio_set_direction(mapping->num, RG_GPIO_INPUT);
    }
#elif defined(RG_TARGET_T_DECK_PLUS)
    rg_i2c_write_byte(T_DECK_KBD_ADDRESS, -1, T_DECK_KBD_MODE_RAW_CMD);
#endif
    UPDATE_GLOBAL_MAP(keymap_i2c);
#endif

#if defined(RG_GAMEPAD_KBD_MAP)
    RG_LOGI("Initializing KBD gamepad driver...");
    UPDATE_GLOBAL_MAP(keymap_kbd);
#endif

#if defined(RG_GAMEPAD_SERIAL_MAP)
    RG_LOGI("Initializing SERIAL gamepad driver...");
    gpio_set_direction(RG_GPIO_GAMEPAD_CLOCK, GPIO_MODE_OUTPUT);
    gpio_set_direction(RG_GPIO_GAMEPAD_LATCH, GPIO_MODE_OUTPUT);
    gpio_set_direction(RG_GPIO_GAMEPAD_DATA, GPIO_MODE_INPUT);
    gpio_set_level(RG_GPIO_GAMEPAD_LATCH, 0);
    gpio_set_level(RG_GPIO_GAMEPAD_CLOCK, 1);
    UPDATE_GLOBAL_MAP(keymap_serial);
#endif

#if defined(RG_ENABLE_USB_HID_HOST)
    if (!rg_usb_msc_boot_requested())
    {
        RG_LOGI("Initializing USB HID gamepad, keyboard and mouse host...");
        rg_usb_hid_init();
        gamepad_mapped |= RG_KEY_ALL;
    }
#endif

#if defined(RG_ENABLE_USB_XINPUT)
    if (!rg_usb_msc_boot_requested())
    {
        RG_LOGI("Initializing USB Xbox controller host...");
        rg_usb_xinput_init();
        gamepad_mapped |= RG_KEY_ALL;
    }
#endif


#if RG_BATTERY_DRIVER == 1 /* ADC */
    RG_LOGI("Initializing ADC battery driver...");
    if (RG_BATTERY_ADC_UNIT == ADC_UNIT_1)
    {
        adc1_config_width(ADC_WIDTH_MAX - 1); // there is no adc2_config_width
        adc1_config_channel_atten(RG_BATTERY_ADC_CHANNEL, ADC_ATTEN_DB_11);
        esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_MAX - 1, 1100, &adc_chars);
    }
    else if (RG_BATTERY_ADC_UNIT == ADC_UNIT_2)
    {
        adc2_config_channel_atten(RG_BATTERY_ADC_CHANNEL, ADC_ATTEN_DB_11);
        esp_adc_cal_characterize(ADC_UNIT_2, ADC_ATTEN_DB_11, ADC_WIDTH_MAX - 1, 1100, &adc_chars);
    }
    else
    {
        RG_LOGE("Only ADC1 and ADC2 are supported for ADC battery driver!");
    }
#endif

    load_player_assignments();

    // The first read returns bogus data in some drivers, waste it.
    rg_input_read_gamepad_raw(NULL);

    // Start background polling
    rg_task_create("rg_input", &input_task, NULL, 3 * 1024, RG_TASK_PRIORITY_6, RG_TASK_AFFINITY_IO);
    while (gamepad_state[RG_PLAYER_1] == (uint32_t)-1)
        rg_task_yield();
    RG_LOGI("Input ready. state=" PRINTF_BINARY_16 "\n", PRINTF_BINVAL_16(gamepad_state[RG_PLAYER_1]));
}

void rg_input_deinit(void)
{
    input_task_running = false;
#if defined(RG_ENABLE_USB_HID_HOST)
    rg_usb_hid_deinit();
#endif
#if defined(RG_ENABLE_USB_XINPUT)
    rg_usb_xinput_deinit();
#endif
    // while (gamepad_state != -1)
    //     rg_task_yield();
    RG_LOGI("Input terminated.\n");
}

bool rg_input_key_is_present(rg_key_t mask)
{
    return (gamepad_mapped & mask) == mask;
}

uint32_t rg_input_read_gamepad_unfiltered(void)
{
#ifdef RG_TARGET_SDL2
    SDL_PumpEvents();
#endif
    return gamepad_state[RG_PLAYER_1];
}

uint32_t rg_input_read_gamepad(void)
{
    return rg_system_filter_screen_timeout_input(rg_input_read_gamepad_unfiltered());
}

uint32_t rg_input_read_gamepad_player(rg_player_t player)
{
    if (player == RG_PLAYER_1)
        return rg_input_read_gamepad();
    if (player < 0 || player >= RG_PLAYER_COUNT)
        return 0;
    return gamepad_state[player];
}

bool rg_input_key_is_pressed(rg_key_t mask)
{
    return (bool)(rg_input_read_gamepad() & mask);
}

bool rg_input_wait_for_key(rg_key_t mask, bool pressed, int timeout_ms)
{
    int64_t expiration = timeout_ms < 0 ? INT64_MAX : (rg_system_timer() + timeout_ms * 1000);
    while (rg_input_key_is_pressed(mask) != pressed)
    {
        if (rg_system_timer() > expiration)
            return false;
        rg_task_delay(10);
    }
    return true;
}

rg_battery_t rg_input_read_battery(void)
{
    return battery_state;
}

const char *rg_input_get_key_name(rg_key_t key)
{
    switch (key)
    {
    case RG_KEY_UP: return "Up";
    case RG_KEY_RIGHT: return "Right";
    case RG_KEY_DOWN: return "Down";
    case RG_KEY_LEFT: return "Left";
    case RG_KEY_SELECT: return "Select";
    case RG_KEY_START: return "Start";
    case RG_KEY_MENU: return "Menu";
    case RG_KEY_OPTION: return "Option";
    case RG_KEY_A: return "A";
    case RG_KEY_B: return "B";
    case RG_KEY_X: return "X";
    case RG_KEY_Y: return "Y";
    case RG_KEY_L: return "Left Shoulder";
    case RG_KEY_R: return "Right Shoulder";
    case RG_KEY_NONE: return "None";
    default: return "Unknown";
    }
}

bool rg_input_source_connected(rg_input_source_t source)
{
    if (source < 0 || source >= RG_INPUT_SOURCE_COUNT)
        return false;
    return source_connected(source);
}

const char *rg_input_source_name(rg_input_source_t source)
{
    switch (source)
    {
    case RG_INPUT_SOURCE_BUILTIN: return "Built-in controls";
    case RG_INPUT_SOURCE_USB_GAMEPAD_1: return "USB Gamepad 1";
    case RG_INPUT_SOURCE_USB_GAMEPAD_2: return "USB Gamepad 2";
    case RG_INPUT_SOURCE_USB_KEYBOARD: return "USB Keyboard";
    case RG_INPUT_SOURCE_USB_MOUSE: return "USB Mouse";
    case RG_INPUT_SOURCE_XINPUT_1: return "Xbox Controller 1";
    case RG_INPUT_SOURCE_XINPUT_2: return "Xbox Controller 2";
    default: return "Unknown";
    }
}

int rg_input_source_get_assignment(rg_input_source_t source)
{
    if (source < 0 || source >= RG_INPUT_SOURCE_COUNT)
        return RG_INPUT_PLAYER_OFF;
    return source_assignment[source];
}

void rg_input_source_set_assignment(rg_input_source_t source, int assignment)
{
    if (source < 0 || source >= RG_INPUT_SOURCE_COUNT)
        return;
    if (assignment != RG_INPUT_PLAYER_AUTO && assignment != RG_INPUT_PLAYER_OFF &&
        (assignment < 0 || assignment >= RG_PLAYER_COUNT))
        return;
    source_assignment[source] = (int8_t)assignment;
    char key[24];
    snprintf(key, sizeof(key), "InputPlayer_%d", source);
    rg_settings_set_number(NS_GLOBAL, key, assignment);
    rg_settings_commit();
}

int rg_input_source_get_player(rg_input_source_t source)
{
    if (source < 0 || source >= RG_INPUT_SOURCE_COUNT)
        return RG_INPUT_PLAYER_OFF;
    return source_resolved_player[source];
}
