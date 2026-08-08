// Target definition
#define RG_TARGET_NAME             "ESP32-S3-N8R2-ST7789V2"
#define RG_ENABLE_USB_HID_HOST     1
#define RG_ENABLE_USB_XINPUT       1
#define RG_ENABLE_USB_MSC          1

// Keep emulator/game logic on core 0 and IO/audio helper tasks on core 1.
#define RG_TASK_AFFINITY_MAIN      0
#define RG_TASK_AFFINITY_IO        1
#define RG_TASK_AFFINITY_AUDIO     1
#define RG_TASK_AFFINITY_SYSTEM    -1

// Storage
#define RG_STORAGE_ROOT             "/sd"
#define RG_STORAGE_SDSPI_HOST       SPI3_HOST
#define RG_STORAGE_SDSPI_SPEED      SDMMC_FREQ_DEFAULT

// Audio
#define RG_AUDIO_USE_INT_DAC        0
#define RG_AUDIO_USE_EXT_DAC        1
#define RG_AUDIO_USE_HEADPHONE_JACK 1   // Headphone DAC shares the I2S bus with the speaker amp
#define RG_AUDIO_DMA_BUFFER_COUNT   8   // 48 ms reserve at 32 kHz absorbs heavy render spikes
#define RG_AUDIO_DMA_BUFFER_LENGTH  192
#define RG_AUDIO_I2S_INTR_FLAGS     ESP_INTR_FLAG_LEVEL2

// Video
#define RG_SCREEN_DRIVER            0   // ILI9341/ST7789-compatible SPI command driver
#define RG_SCREEN_HOST              SPI2_HOST
#define RG_SCREEN_SPEED             SPI_MASTER_FREQ_80M
#define RG_SCREEN_BACKLIGHT         1
// The ST7789V2 controller RAM is 320x240 in landscape. The 1.69-inch glass
// exposes 280x240 pixels, centered with a 20-pixel leading column offset.
#define RG_SCREEN_WIDTH             300
#define RG_SCREEN_HEIGHT            240
#define RG_SCREEN_ROTATE            0
#define RG_SCREEN_VISIBLE_AREA      {20, 0, 0, 0}
#define RG_SCREEN_SAFE_AREA         {20, 0, 0, 0}

#define ST7789_MADCTL_MV            0x20
#define ST7789_MADCTL_MY            0x80

#define RG_SCREEN_INIT()                                                                                         \
    ILI9341_CMD(0x36, ST7789_MADCTL_MY | ST7789_MADCTL_MV); /* Landscape RGB, rotated 180 degrees */            \
    ILI9341_CMD(0xB2, 0x0C, 0x0C, 0x00, 0x33, 0x33);       /* Porch control */                                  \
    ILI9341_CMD(0xB7, 0x35);                               /* Gate control */                                    \
    ILI9341_CMD(0xBB, 0x19);                               /* VCOM setting */                                    \
    ILI9341_CMD(0xC0, 0x2C);                               /* LCM control */                                     \
    ILI9341_CMD(0xC2, 0x01);                               /* VDV and VRH command enable */                      \
    ILI9341_CMD(0xC3, 0x12);                               /* VRH set */                                         \
    ILI9341_CMD(0xC4, 0x20);                               /* VDV set */                                         \
    ILI9341_CMD(0xC6, 0x0F);                               /* Frame rate control */                              \
    ILI9341_CMD(0xD0, 0xA4, 0xA1);                         /* Power control */                                   \
    ILI9341_CMD(0xE0, 0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23);       \
    ILI9341_CMD(0xE1, 0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23);       \
    ILI9341_CMD(0x21);                                     /* Display inversion on */

// Input
#define RG_GAMEPAD_GPIO_MAP {\
    {RG_KEY_A,      .num = GPIO_NUM_45, .pullup = 1, .level = 0},\
    {RG_KEY_B,      .num = GPIO_NUM_47, .pullup = 1, .level = 0},\
    {RG_KEY_SELECT, .num = GPIO_NUM_16, .pullup = 1, .level = 0},\
    {RG_KEY_START,  .num = GPIO_NUM_15, .pullup = 1, .level = 0},\
    {RG_KEY_MENU,   .num = GPIO_NUM_2,  .pullup = 1, .level = 0},\
    {RG_KEY_OPTION, .num = GPIO_NUM_7,  .pullup = 1, .level = 0},\
    {RG_KEY_UP,     .num = GPIO_NUM_17, .pullup = 1, .level = 0},\
    {RG_KEY_DOWN,   .num = GPIO_NUM_3,  .pullup = 1, .level = 0},\
    {RG_KEY_LEFT,   .num = GPIO_NUM_8,  .pullup = 1, .level = 0},\
    {RG_KEY_RIGHT,  .num = GPIO_NUM_18, .pullup = 1, .level = 0},\
}

#define RG_GAMEPAD_VIRT_MAP {\
    {RG_KEY_MENU, .src = RG_KEY_START | RG_KEY_SELECT},\
}

#define RG_RECOVERY_BTN             RG_KEY_MENU

// Status LED
#define RG_GPIO_LED                 GPIO_NUM_48
#define RG_GPIO_LED_WS2812          1

// Haptic feedback motor driver
#define RG_GPIO_VIBRATOR            GPIO_NUM_38

// No battery ADC was provided for this board.
#define RG_BATTERY_DRIVER           0

// SPI Display
#define RG_GPIO_LCD_MISO            GPIO_NUM_NC
#define RG_GPIO_LCD_MOSI            GPIO_NUM_13
#define RG_GPIO_LCD_CLK             GPIO_NUM_14
#define RG_GPIO_LCD_CS              GPIO_NUM_10
#define RG_GPIO_LCD_DC              GPIO_NUM_11
#define RG_GPIO_LCD_BCKL            GPIO_NUM_21
#define RG_GPIO_LCD_RST             GPIO_NUM_12

// SPI SD Card
#define RG_GPIO_SDSPI_MISO          GPIO_NUM_39
#define RG_GPIO_SDSPI_MOSI          GPIO_NUM_41
#define RG_GPIO_SDSPI_CLK           GPIO_NUM_40
#define RG_GPIO_SDSPI_CS            GPIO_NUM_42

// External I2S DAC. The bus is shared by two slaves: a MAX98357A driving the speaker and a
// PCM5102A driving the 3.5mm jack. Only one of them is enabled at a time.
#define RG_GPIO_SND_I2S_BCK         GPIO_NUM_5
#define RG_GPIO_SND_I2S_WS          GPIO_NUM_4
#define RG_GPIO_SND_I2S_DATA        GPIO_NUM_6

// Output routing. AMP_ENABLE drives MAX98357A SD_MODE (through 470k, see README), HP_ENABLE
// drives PCM5102A XSMT (or a headphone amp shutdown). Both are active high.
#define RG_GPIO_SND_AMP_ENABLE      GPIO_NUM_9
#define RG_GPIO_SND_HP_ENABLE       GPIO_NUM_46
// Jack detect: the 4th (ring2) contact, shorted to the plug's sleeve when one is inserted.
// Idle state is the internal pull-up, so a board without the mod correctly reports no headphones.
#define RG_GPIO_SND_HP_DETECT       GPIO_NUM_1
#define RG_GPIO_SND_HP_DETECT_LEVEL 0

// Updater
#define RG_UPDATER_ENABLE               1
#define RG_UPDATER_APPLICATION          RG_APP_FACTORY
#define RG_UPDATER_DOWNLOAD_LOCATION    RG_STORAGE_ROOT "/retro-go/firmware"
