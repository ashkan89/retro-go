// Target definition
#define RG_TARGET_NAME             "ESP32-S3-N8R2-ST7796"

// Storage
#define RG_STORAGE_ROOT             "/sd"
#define RG_STORAGE_SDSPI_HOST       SPI3_HOST
#define RG_STORAGE_SDSPI_SPEED      SDMMC_FREQ_DEFAULT

// Audio
#define RG_AUDIO_USE_INT_DAC        0
#define RG_AUDIO_USE_EXT_DAC        1

// Video
#define RG_SCREEN_DRIVER            0   // 0 = ILI9341/ST7789 compatible SPI command driver
#define RG_SCREEN_HOST              SPI2_HOST
#define RG_SCREEN_SPEED             SPI_MASTER_FREQ_40M
#define RG_SCREEN_BACKLIGHT         0
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
    {RG_KEY_A,      .num = GPIO_NUM_45, .pullup = 1, .level = 0},\
    {RG_KEY_B,      .num = GPIO_NUM_47, .pullup = 1, .level = 0},\
    {RG_KEY_SELECT, .num = GPIO_NUM_16, .pullup = 1, .level = 0},\
    {RG_KEY_START,  .num = GPIO_NUM_15, .pullup = 1, .level = 0},\
    {RG_KEY_UP,     .num = GPIO_NUM_17, .pullup = 1, .level = 0},\
    {RG_KEY_DOWN,   .num = GPIO_NUM_3,  .pullup = 1, .level = 0},\
    {RG_KEY_LEFT,   .num = GPIO_NUM_8,  .pullup = 1, .level = 0},\
    {RG_KEY_RIGHT,  .num = GPIO_NUM_18, .pullup = 1, .level = 0},\
}
#define RG_GAMEPAD_VIRT_MAP {\
    {RG_KEY_MENU, .src = RG_KEY_START | RG_KEY_SELECT},\
}

#define RG_RECOVERY_BTN             RG_KEY_MENU

// No battery ADC was provided for this board.
#define RG_BATTERY_DRIVER           0

// SPI Display
#define RG_GPIO_LCD_MISO            GPIO_NUM_NC
#define RG_GPIO_LCD_MOSI            GPIO_NUM_13
#define RG_GPIO_LCD_CLK             GPIO_NUM_14
#define RG_GPIO_LCD_CS              GPIO_NUM_10
#define RG_GPIO_LCD_DC              GPIO_NUM_11
#define RG_GPIO_LCD_RST             GPIO_NUM_12

// SPI SD Card
#define RG_GPIO_SDSPI_MISO          GPIO_NUM_39
#define RG_GPIO_SDSPI_MOSI          GPIO_NUM_41
#define RG_GPIO_SDSPI_CLK           GPIO_NUM_40
#define RG_GPIO_SDSPI_CS            GPIO_NUM_42

// External I2S DAC (MAX98357A)
#define RG_GPIO_SND_I2S_BCK         GPIO_NUM_5
#define RG_GPIO_SND_I2S_WS          GPIO_NUM_4
#define RG_GPIO_SND_I2S_DATA        GPIO_NUM_6
