#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <driver/ledc.h>

#define SPI_TRANSACTION_COUNT 8
#if CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S3
#define SPI_BUFFER_COUNT 3
#else
#define SPI_BUFFER_COUNT 5
#endif
#define SPI_BUFFER_LENGTH (LCD_BUFFER_LENGTH * sizeof(uint16_t))

static spi_device_handle_t spi_dev;
static SemaphoreHandle_t lcd_mutex;
static bool spi_owns_bus;
// Foreground queue and buffer ownership are serialized by lcd_mutex.
static DRAM_ATTR spi_transaction_ext_t spi_transactions[SPI_TRANSACTION_COUNT];
static uint16_t *spi_buffers[SPI_BUFFER_COUNT];
static unsigned spi_next_transaction, spi_pending, spi_free_buffers;
static int lcd_left, lcd_right, lcd_top, lcd_bottom;

#define ILI9341_CMD(cmd, data...) do {                  \
    const uint8_t c = cmd, x[] = {data};                \
    spi_queue_transaction(&c, 1, 0);                    \
    if (sizeof(x))                                     \
        spi_queue_transaction(x, sizeof(x), 1);         \
} while (0)

static inline void lcd_lock(void)
{
    xSemaphoreTakeRecursive(lcd_mutex, portMAX_DELAY);
}

static inline void lcd_unlock(void)
{
    xSemaphoreGiveRecursive(lcd_mutex);
}

static void spi_reclaim_transaction(void)
{
    spi_transaction_t *t;
    if (spi_device_get_trans_result(spi_dev, &t, pdMS_TO_TICKS(2500)) != ESP_OK)
        RG_PANIC("LCD SPI completion failed");
    if ((uintptr_t)t->user & 2)
        spi_buffers[spi_free_buffers++] = (uint16_t *)t->tx_buffer;
    --spi_pending;
}

static inline uint16_t *spi_take_buffer(void)
{
    while (!spi_free_buffers)
        spi_reclaim_transaction();
    return spi_buffers[--spi_free_buffers];
}

static inline void spi_queue_transaction(const void *data, size_t length, uintptr_t type)
{
    // ESP-IDF delivers results in submission order. Reclaim at the producer
    // when the ring or buffer pool fills, keeping the ISR-fed queue running.
    if (spi_pending == SPI_TRANSACTION_COUNT)
        spi_reclaim_transaction();
    spi_transaction_ext_t *t = &spi_transactions[spi_next_transaction];
    *t = (spi_transaction_ext_t){.base.user = (void *)type};

    if (!(type & 2) && length <= 4)
    {
        // Clock short command/parameter bytes in the hardware address phase.
        // The MOSI data phase stays empty, bypassing DMA setup and bounce buffers.
        const uint8_t *bytes = data;
        uint32_t address = 0;
        for (size_t i = 0; i < length; ++i)
            address = (address << 8) | bytes[i];
        t->base.addr = address;
        t->base.flags = SPI_TRANS_VARIABLE_ADDR;
        t->address_bits = length * 8;
    }
    else
    {
        t->base.length = length * 8;
        t->base.tx_buffer = (type & 2) ? data : memcpy(spi_take_buffer(), data, length);
        t->base.user = (void *)(type | 2);
    }

    if (spi_device_queue_trans(spi_dev, &t->base, pdMS_TO_TICKS(2500)) != ESP_OK)
        RG_PANIC("LCD SPI submission failed");
    spi_next_transaction = (spi_next_transaction + 1) % SPI_TRANSACTION_COUNT;
    ++spi_pending;
}

IRAM_ATTR
static void spi_pre_transfer_cb(spi_transaction_t *t)
{
    gpio_set_level(RG_GPIO_LCD_DC, (uintptr_t)t->user & 1);
}

static void spi_drain(void)
{
    while (spi_pending)
        spi_reclaim_transaction();
}

static void spi_init(void)
{
    lcd_mutex = xSemaphoreCreateRecursiveMutex();
    RG_ASSERT(lcd_mutex, "Unable to allocate LCD mutex");
    lcd_left = lcd_right = lcd_top = lcd_bottom = -1;
    spi_next_transaction = spi_pending = spi_free_buffers = 0;

    const spi_bus_config_t buscfg = {
        .miso_io_num = RG_GPIO_LCD_MISO,
        .mosi_io_num = RG_GPIO_LCD_MOSI,
        .sclk_io_num = RG_GPIO_LCD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_BUFFER_LENGTH,
#if (CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
        // Install the DMA completion ISR on the display core, rather than the
        // initializing emulator core. Keep older IDF ports on their default.
#if RG_TASK_AFFINITY_DISPLAY == 1
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_1,
#elif RG_TASK_AFFINITY_DISPLAY == 0
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_0,
#endif
#endif
    };

    const spi_device_interface_config_t devcfg = {
        .clock_speed_hz = RG_SCREEN_SPEED,   // Typically SPI_MASTER_FREQ_40M or SPI_MASTER_FREQ_80M
        .mode = 0,                           // SPI mode 0
        .spics_io_num = RG_GPIO_LCD_CS,      // CS pin
        .queue_size = SPI_TRANSACTION_COUNT,
        .pre_cb = &spi_pre_transfer_cb,
        .flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY,
    };

    esp_err_t ret = spi_bus_initialize(RG_SCREEN_HOST, &buscfg, SPI_DMA_CH_AUTO);
    RG_ASSERT(ret == ESP_OK || ret == ESP_ERR_INVALID_STATE, "spi_bus_initialize failed.");
    spi_owns_bus = ret == ESP_OK;

    ret = spi_bus_add_device(RG_SCREEN_HOST, &devcfg, &spi_dev);
    RG_ASSERT(ret == ESP_OK, "spi_bus_add_device failed.");

    lcd_buffer_pixels = LCD_BUFFER_LENGTH;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
    size_t max_transfer;
    if (!spi_owns_bus && spi_bus_get_max_transaction_len(RG_SCREEN_HOST, &max_transfer) == ESP_OK)
        lcd_buffer_pixels = RG_MIN((size_t)LCD_BUFFER_LENGTH, max_transfer / sizeof(uint16_t));
#endif
    RG_ASSERT(lcd_buffer_pixels >= RG_SCREEN_WIDTH, "LCD SPI bus cannot transfer a full scanline");
    const size_t buffer_bytes = lcd_buffer_pixels * sizeof(uint16_t);
    while (spi_free_buffers < SPI_BUFFER_COUNT)
    {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
        void *buffer = spi_bus_dma_memory_alloc(RG_SCREEN_HOST, buffer_bytes, MALLOC_CAP_INTERNAL);
        RG_ASSERT(buffer, "Unable to allocate LCD DMA buffer");
#else
        void *buffer = rg_alloc(buffer_bytes, MEM_DMA | MEM_FAST);
#endif
        spi_buffers[spi_free_buffers++] = buffer;
    }

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    int clock_khz;
    if (spi_device_get_actual_freq(spi_dev, &clock_khz) == ESP_OK)
        RG_LOGI("LCD SPI: %d kHz, %d buffers of %u bytes", clock_khz, SPI_BUFFER_COUNT, (unsigned)buffer_bytes);
#endif
}

static void spi_deinit(void)
{
    spi_drain();
    RG_ASSERT(spi_bus_remove_device(spi_dev) == ESP_OK, "Unable to remove LCD SPI device");
    // Another device, such as the SD card, may still be attached.
    if (spi_owns_bus)
        spi_bus_free(RG_SCREEN_HOST);
    while (spi_free_buffers)
        free(spi_buffers[--spi_free_buffers]);
    vSemaphoreDelete(lcd_mutex);
}

static void lcd_set_backlight(float percent)
{
    float level = RG_MIN(RG_MAX(percent / 100.f, 0), 1.f);
    int error_code = 0;

#if defined(RG_GPIO_LCD_BCKL)
    error_code = ledc_set_fade_time_and_start(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0x1FFF * level, 50, 0);
#endif

    if (error_code)
        RG_LOGE("failed setting backlight to %d%% (0x%02X)\n", (int)(100 * level), error_code);
    else
        RG_LOGI("backlight set to %d%%\n", (int)(100 * level));
}

static void lcd_set_window(int left, int top, int width, int height)
{
    int right = left + width - 1;
    int bottom = top + height - 1;

    if (left < 0 || top < 0 || right >= display.screen.real_width || bottom >= display.screen.real_height)
        RG_LOGW("Bad lcd window (x0=%d, y0=%d, x1=%d, y1=%d)\n", left, top, right, bottom);

    if (left != lcd_left || right != lcd_right)
    {
        ILI9341_CMD(0x2A, left >> 8, left & 0xff, right >> 8, right & 0xff);
        lcd_left = left;
        lcd_right = right;
    }
    if (top != lcd_top || bottom != lcd_bottom)
    {
        ILI9341_CMD(0x2B, top >> 8, top & 0xff, bottom >> 8, bottom & 0xff);
        lcd_top = top;
        lcd_bottom = bottom;
    }
    ILI9341_CMD(0x2C);                                                   // Memory write
}

static inline uint16_t *lcd_get_buffer(size_t length)
{
    RG_ASSERT_ARG(length <= lcd_buffer_pixels);
    return spi_take_buffer();
}

static inline void lcd_send_buffer(uint16_t *buffer, size_t length)
{
    if (length > 0)
        spi_queue_transaction(buffer, length * sizeof(*buffer), 3);
    else
        spi_buffers[spi_free_buffers++] = buffer;
}

static void lcd_sync(void)
{
    // Unused for SPI LCD
}

#ifdef RG_P4_SELFTEST
static void lcd_diagnostic_sync(void)
{
    spi_drain();
}
#endif

static void lcd_init(void)
{
#ifdef RG_GPIO_LCD_BCKL
    // Initialize backlight at 0% to avoid the lcd reset flash
    ledc_timer_config(&(ledc_timer_config_t){
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
    });
    ledc_channel_config(&(ledc_channel_config_t){
        .gpio_num = RG_GPIO_LCD_BCKL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    #ifdef RG_GPIO_LCD_BCKL_INVERT
        .flags.output_invert = 1,
    #endif
    });
    ledc_fade_func_install(0);
#endif

    spi_init();

    // Setup Data/Command line
    gpio_set_direction(RG_GPIO_LCD_DC, GPIO_MODE_OUTPUT);
    gpio_set_level(RG_GPIO_LCD_DC, 1);

#if defined(RG_GPIO_LCD_RST)
    gpio_set_direction(RG_GPIO_LCD_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(RG_GPIO_LCD_RST, 0);
    rg_usleep(100 * 1000);
    gpio_set_level(RG_GPIO_LCD_RST, 1);
    rg_usleep(10 * 1000);
#endif

    ILI9341_CMD(0x01);       // Reset
    spi_drain();
    rg_usleep(120 * 1000);   // Allow the controller to finish software reset
    ILI9341_CMD(0x3A, 0X55); // COLMOD (Pixel Format Set RGB565 65k)
#if defined(RG_SCREEN_ROTATION) && defined(RG_SCREEN_RGB_BGR)
    // The rotation is designed so that the user can simply try all values 0-7 to find what works.
    // It's simpler than trying to explain the MADCTL register bits, combined with hardware variations...
    ILI9341_CMD(0x36, (RG_SCREEN_RGB_BGR ? 0x08 : 0x00) | (RG_SCREEN_ROTATION << 5)); // MADCTL (0x08=BGR, 0x20=MV, 0x40=MX, 0x80=MY)
#endif
#ifdef RG_SCREEN_INIT
    RG_SCREEN_INIT();
#else
    #warning "LCD init sequence is not defined for this device!"
#endif
    ILI9341_CMD(0x11);    // Exit Sleep
    spi_drain();
    rg_usleep(120 * 1000); // Allow power supplies and panel timing to stabilize
    ILI9341_CMD(0x29);    // Display on
}

static void lcd_deinit(void)
{
#ifdef RG_SCREEN_DEINIT
    RG_SCREEN_DEINIT();
#endif
    spi_deinit();
    // gpio_reset_pin(RG_GPIO_LCD_BCKL);
    // gpio_reset_pin(RG_GPIO_LCD_DC);
}

const rg_display_driver_t rg_display_driver_ili9341 = {
    .name = "ili9341",
};
