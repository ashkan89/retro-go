// Target definition
#define RG_TARGET_NAME             "ESP32-P4-N16R32-ST7796"
#define RG_ENABLE_USB_HID_HOST     1
#define RG_ENABLE_USB_XINPUT       1
#define RG_ENABLE_USB_MSC          1

// Wireless-Tag WT9932P4-TINY with an external ST7796: see docs/README.md.
// Wi-Fi uses an ESP32-WROOM-32D ESP-Hosted SDIO companion; see docs/WIFI.md.

// Keep emulator/game logic on core 0 and IO/audio helper tasks on core 1.
#define RG_TASK_AFFINITY_MAIN      0
#define RG_TASK_AFFINITY_IO        1
#define RG_TASK_AFFINITY_AUDIO     1
#define RG_TASK_AFFINITY_SYSTEM    -1

// Storage
#define RG_STORAGE_ROOT             "/sd"
#define RG_STORAGE_SDMMC_HOST       SDMMC_HOST_SLOT_0
#define RG_STORAGE_SDMMC_SPEED      SDMMC_FREQ_HIGHSPEED // 40 MHz, negotiated down for slower cards
#define RG_STORAGE_SDMMC_WIDTH      4
// The onboard J5 TF socket and its I/O bank are powered by LDO4 at 3.3 V.
#define RG_STORAGE_SD_IO_LDO_CHANNEL 4
#define RG_STORAGE_SD_IO_VOLTAGE_MV 3300

// Audio
#define RG_AUDIO_USE_INT_DAC        0
#define RG_AUDIO_USE_EXT_DAC        1
#define RG_AUDIO_USE_HEADPHONE_JACK 1
#define RG_AUDIO_DMA_BUFFER_COUNT   8   // 48 ms reserve at 32 kHz absorbs heavy render spikes
#define RG_AUDIO_DMA_BUFFER_LENGTH  192
#define RG_AUDIO_I2S_INTR_FLAGS     ESP_INTR_FLAG_LEVEL2
#define RG_AUDIO_VOLUME_RAMP_MS     5
#define RG_AUDIO_TIMING_DIAGNOSTICS 0

// Video
#define RG_SCREEN_DRIVER            0   // 0 = ILI9341/ST7789 compatible SPI command driver
#define RG_SCREEN_HOST              SPI2_HOST
#define RG_SCREEN_SPEED             SPI_MASTER_FREQ_80M
#define RG_SCREEN_BUFFER_LINES      16
#define RG_DISPLAY_ASYNC_FRAMES     1
#define RG_DISPLAY_PPA_VIEWPORT     1   // Match Smooth/Fit and Full to hardware scale precision
#define RG_SCREEN_BACKLIGHT         1
#define RG_SCREEN_WIDTH             480
#define RG_SCREEN_HEIGHT            320
#define RG_SCREEN_ROTATE            0
#define RG_SCREEN_VISIBLE_AREA      {0, 0, 0, 0}
#define RG_SCREEN_SAFE_AREA         {0, 0, 0, 0}

#define ST7796_MADCTL_MY            0x80
#define ST7796_MADCTL_MX            0x40
#define ST7796_MADCTL_MV            0x20
#define ST7796_MADCTL_BGR           0x08

#define RG_SCREEN_INIT()                                                                                         \
    ILI9341_CMD(0xF0, 0xC3);                 /* Command Set Control */                                           \
    ILI9341_CMD(0xF0, 0x96);                                                                                     \
    ILI9341_CMD(0x36, ST7796_MADCTL_MY | ST7796_MADCTL_MX | ST7796_MADCTL_MV | ST7796_MADCTL_BGR); /* 180 deg */ \
    ILI9341_CMD(0xB4, 0x01);                 /* Display inversion control */                                     \
    ILI9341_CMD(0xB6, 0x80, 0x02, 0x3B);     /* Display function control */                                      \
    ILI9341_CMD(0xE8, 0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33);                                           \
    ILI9341_CMD(0xC1, 0x06);                 /* Power control 2 */                                               \
    ILI9341_CMD(0xC2, 0xA7);                 /* Power control 3 */                                               \
    ILI9341_CMD(0xC5, 0x18);                 /* VCOM control */                                                  \
    ILI9341_CMD(0xE0, 0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F, 0x54, 0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B);       \
    ILI9341_CMD(0xE1, 0xE0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2B, 0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B);       \
    ILI9341_CMD(0xF0, 0x3C);                                                                                     \
    ILI9341_CMD(0xF0, 0x69);

// Input
#define RG_GAMEPAD_GPIO_MAP {\
    {RG_KEY_A,      .num = GPIO_NUM_2, .pullup = 1, .level = 0},\
    {RG_KEY_B,      .num = GPIO_NUM_3, .pullup = 1, .level = 0},\
    {RG_KEY_SELECT, .num = GPIO_NUM_4, .pullup = 1, .level = 0},\
    {RG_KEY_START,  .num = GPIO_NUM_5, .pullup = 1, .level = 0},\
    {RG_KEY_MENU,   .num = GPIO_NUM_6,  .pullup = 1, .level = 0},\
    {RG_KEY_OPTION, .num = GPIO_NUM_11,  .pullup = 1, .level = 0},\
    {RG_KEY_UP,     .num = GPIO_NUM_12, .pullup = 1, .level = 0},\
    {RG_KEY_DOWN,   .num = GPIO_NUM_13,  .pullup = 1, .level = 0},\
    {RG_KEY_LEFT,   .num = GPIO_NUM_14,  .pullup = 1, .level = 0},\
    {RG_KEY_RIGHT,  .num = GPIO_NUM_15, .pullup = 1, .level = 0},\
}

#define RG_GAMEPAD_VIRT_MAP {\
    {RG_KEY_MENU, .src = RG_KEY_START | RG_KEY_SELECT},\
}

#define RG_RECOVERY_BTN             RG_KEY_MENU

// Optional external accessories carried over from the S3 build.
#define RG_GPIO_LED                 GPIO_NUM_51
#define RG_GPIO_LED_WS2812          1
// The onboard WS2812 uses GRB byte order, as in Wireless-Tag's BSP.
#define RG_GPIO_VIBRATOR            GPIO_NUM_18

// Same 100k/100k 1S battery divider; P4 ADC1 channel 0 is GPIO16.
#define RG_BATTERY_DRIVER          1
#define RG_BATTERY_ADC_GPIO        GPIO_NUM_16
#define RG_BATTERY_ADC_UNIT        ADC_UNIT_1
#define RG_BATTERY_ADC_CHANNEL     ADC_CHANNEL_0
#define RG_BATTERY_DIVIDER_RATIO   2.f
#define RG_BATTERY_CALC_VOLTAGE(mv) ((mv) * RG_BATTERY_DIVIDER_RATIO * 0.001f)
#define RG_BATTERY_CALC_PERCENT(mv) (((mv) * RG_BATTERY_DIVIDER_RATIO - 3300.f) * (100.f / 900.f))
#define RG_BATTERY_IS_PRESENT(mv)   ((mv) >= 1000)

// SPI Display
#define RG_GPIO_LCD_MISO            GPIO_NUM_NC
#define RG_GPIO_LCD_MOSI            GPIO_NUM_29
#define RG_GPIO_LCD_CLK             GPIO_NUM_30
#define RG_GPIO_LCD_CS              GPIO_NUM_28
#define RG_GPIO_LCD_DC              GPIO_NUM_31
#define RG_GPIO_LCD_BCKL            GPIO_NUM_33
#define RG_GPIO_LCD_RST             GPIO_NUM_32

// Onboard J5 TF socket: slot 0's native IOMUX pins, no external SD wiring.
#define RG_GPIO_SDMMC_CLK          GPIO_NUM_43
#define RG_GPIO_SDMMC_CMD          GPIO_NUM_44
#define RG_GPIO_SDMMC_D0           GPIO_NUM_39
#define RG_GPIO_SDMMC_D1           GPIO_NUM_40
#define RG_GPIO_SDMMC_D2           GPIO_NUM_41
#define RG_GPIO_SDMMC_D3           GPIO_NUM_42

// Shared I2S bus for the MAX98357A speaker amp and PCM5102A headphone DAC.
#define RG_GPIO_SND_I2S_BCK         GPIO_NUM_20
#define RG_GPIO_SND_I2S_WS          GPIO_NUM_21
#define RG_GPIO_SND_I2S_DATA        GPIO_NUM_22

// MAX98357A SD_MODE and PCM5102A XSMT; same routing circuit as S3.
#define RG_GPIO_SND_AMP_ENABLE      GPIO_NUM_17
#define RG_GPIO_SND_HP_ENABLE       GPIO_NUM_19

// Updater
#define RG_UPDATER_ENABLE               1
#define RG_UPDATER_APPLICATION          RG_APP_FACTORY
#define RG_UPDATER_DOWNLOAD_LOCATION    RG_STORAGE_ROOT "/retro-go/firmware"
