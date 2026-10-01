/*****************************************************************************
 * | File      	 :   rgb_lcd_port.c
 * | Author      :   Waveshare team
 * | Function    :   Hardware underlying interface
 * | Info        :
 *                   RGB LCD driver code
 *----------------
 * |This version :   V1.0
 * | Date        :   2024-11-19
 * | Info        :   Basic version
 *
 ******************************************************************************/

#include "rgb_lcd_port.h"
#include "lvgl_port.h"

static const char *TAG = "rgb_lcd";

// Handle for the RGB LCD panel
static esp_lcd_panel_handle_t panel_handle = NULL; // Declare a handle for the LCD panel

// VSYNC event callback function
IRAM_ATTR static bool rgb_lcd_on_vsync_event(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *edata, void *user_ctx)
{
    return lvgl_port_notify_rgb_vsync();
}

/**
 * @brief Initialize the RGB LCD panel on the ESP32-S3
 *
 * This function configures and initializes an RGB LCD panel driver
 * using the ESP-IDF RGB LCD driver API. It sets up timing parameters,
 * GPIOs, data width, and framebuffer settings for the LCD panel.
 *
 * @return
 *    - ESP_OK: Initialization successful.
 *    - Other error codes: Initialization failed.
 */
esp_lcd_panel_handle_t waveshare_esp32_s3_rgb_lcd_init()
{
    // Log the start of the RGB LCD panel driver installation
    ESP_LOGI(TAG, "Install RGB LCD panel driver");

    // Configuration structure for the RGB LCD panel
    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT, // Use the default clock source
        .timings = {
            .pclk_hz = EXAMPLE_LCD_PIXEL_CLOCK_HZ, // Pixel clock frequency in Hz
            .h_res = EXAMPLE_LCD_H_RES,            // Horizontal resolution (number of pixels per row)
            .v_res = EXAMPLE_LCD_V_RES,            // Vertical resolution (number of rows)
            .hsync_pulse_width = 162,                // Horizontal sync pulse width
            .hsync_back_porch = 152,                 // Horizontal back porch
            .hsync_front_porch = 48,                // Horizontal front porch
            .vsync_pulse_width = 45,                // Vertical sync pulse width
            .vsync_back_porch = 13,                 // Vertical back porch
            .vsync_front_porch = 3,                // Vertical front porch
            .flags = {
                .pclk_active_neg = 1, // Set pixel clock polarity to active low
            },
        },
        .data_width = EXAMPLE_RGB_DATA_WIDTH,                    // Data width for RGB signals
        .bits_per_pixel = EXAMPLE_RGB_BIT_PER_PIXEL,             // Number of bits per pixel (color depth)
        .num_fbs = EXAMPLE_LCD_RGB_BUFFER_NUMS,                  // Number of framebuffers for double/triple buffering
        .bounce_buffer_size_px = EXAMPLE_RGB_BOUNCE_BUFFER_SIZE, // Bounce buffer size in pixels
        .sram_trans_align = 4,                                   // SRAM transaction alignment in bytes
        .psram_trans_align = 64,                                 // PSRAM transaction alignment in bytes
        .hsync_gpio_num = EXAMPLE_LCD_IO_RGB_HSYNC,              // GPIO for horizontal sync signal
        .vsync_gpio_num = EXAMPLE_LCD_IO_RGB_VSYNC,              // GPIO for vertical sync signal
        .de_gpio_num = EXAMPLE_LCD_IO_RGB_DE,                    // GPIO for data enable signal
        .pclk_gpio_num = EXAMPLE_LCD_IO_RGB_PCLK,                // GPIO for pixel clock signal
        .disp_gpio_num = EXAMPLE_LCD_IO_RGB_DISP,                // GPIO for display enable signal
        .data_gpio_nums = {
            // GPIOs for RGB data signals
            EXAMPLE_LCD_IO_RGB_DATA0,  // Data bit 0
            EXAMPLE_LCD_IO_RGB_DATA1,  // Data bit 1
            EXAMPLE_LCD_IO_RGB_DATA2,  // Data bit 2
            EXAMPLE_LCD_IO_RGB_DATA3,  // Data bit 3
            EXAMPLE_LCD_IO_RGB_DATA4,  // Data bit 4
            EXAMPLE_LCD_IO_RGB_DATA5,  // Data bit 5
            EXAMPLE_LCD_IO_RGB_DATA6,  // Data bit 6
            EXAMPLE_LCD_IO_RGB_DATA7,  // Data bit 7
            EXAMPLE_LCD_IO_RGB_DATA8,  // Data bit 8
            EXAMPLE_LCD_IO_RGB_DATA9,  // Data bit 9
            EXAMPLE_LCD_IO_RGB_DATA10, // Data bit 10
            EXAMPLE_LCD_IO_RGB_DATA11, // Data bit 11
            EXAMPLE_LCD_IO_RGB_DATA12, // Data bit 12
            EXAMPLE_LCD_IO_RGB_DATA13, // Data bit 13
            EXAMPLE_LCD_IO_RGB_DATA14, // Data bit 14
            EXAMPLE_LCD_IO_RGB_DATA15, // Data bit 15
        },
        .flags = {
            .fb_in_psram = 1, // Use PSRAM for framebuffers to save internal SRAM
        },
    };

    // Create and register the RGB LCD panel driver with the configuration above
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &panel_handle));

    // Log the initialization of the RGB LCD panel
    ESP_LOGI(TAG, "Initialize RGB LCD panel");

    // Initialize the RGB LCD panel
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    esp_lcd_rgb_panel_event_callbacks_t cbs = {
#if EXAMPLE_RGB_BOUNCE_BUFFER_SIZE > 0
        .on_bounce_frame_finish = rgb_lcd_on_vsync_event, // Callback for bounce frame finish
#else
        .on_vsync = rgb_lcd_on_vsync_event, // Callback for vertical sync
#endif
    };
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel_handle, &cbs, NULL)); // Register event callbacks

    // Return success status
    return panel_handle;
}

void waveshare_get_frame_buffer(void **buf1, void **buf2)
{
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 2, buf1, buf2));
}
/**
 * @brief Turn on the RGB LCD screen backlight.
 *
 * This function enables the backlight of the screen by configuring the IO EXTENSION
 * I/O expander to output mode and setting the backlight pin to high. The IO EXTENSION
 * is controlled via I2C.
 *
 * @return
 *    - ESP_OK: Operation successful.
 */
void waveshare_rgb_lcd_bl_on()
{
    IO_EXTENSION_Output(IO_EXTENSION_IO_2, 1);  // Backlight ON configuration
}

/**
 * @brief Turn off the RGB LCD screen backlight.
 *
 * This function disables the backlight of the screen by configuring the IO EXTENSION
 * I/O expander to output mode and setting the backlight pin to low. The IO EXTENSION
 * is controlled via I2C.
 *
 * @return
 *    - ESP_OK: Operation successful.
 */
void waveshare_rgb_lcd_bl_off()
{
    IO_EXTENSION_Output(IO_EXTENSION_IO_2, 0);  // Backlight OFF configuration
}