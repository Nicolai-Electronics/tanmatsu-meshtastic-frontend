// SPDX-FileCopyrightText: 2025 Nicolai Electronics
// SPDX-License-Identifier: MIT

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/power.h"
#include "bsp/rtc.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_types.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "pax_codecs.h"
#include "bsp/led.h"

#define RADIO_BUFFER 256
#define RADIO_UART   UART_NUM_1
#define RADIO_TX     16  // UART TX going to ESP32-C6
#define RADIO_RX     18  // UART RX coming from ESP32-C6

static const char* TAG = "Terminal";

extern uint8_t const wallpaper_start[] asm("_binary_wallpaper_png_start");
extern uint8_t const wallpaper_end[] asm("_binary_wallpaper_png_end");

static esp_lcd_panel_handle_t    lcd_panel         = NULL;
static esp_lcd_panel_io_handle_t lcd_panel_io      = NULL;
static QueueHandle_t             input_event_queue = NULL;

static pax_buf_t fb = {0};

#define num_lines 17
#define num_chars 60

char line_buffers[num_lines][num_chars];
int  line_index[num_lines];
char input_buffer[num_chars];
uint8_t led_buffer[6 * 3] = {0};

void init(void) {
    for (size_t i = 0; i < num_lines; i++) {
        line_index[i] = i;
    }
}

void add_line(char* text) {
    for (size_t i = 0; i < strlen(text); i++) {
        if (text[i] == '\r' || text[i] == '\n') {
            text[i] = ' ';
        }
    }
    for (size_t i = 0; i < num_lines; i++) {
        line_index[i] = (line_index[i] + 1) % num_lines;
    }
    strncpy(line_buffers[line_index[num_lines - 1]], text, num_chars);
}

void blit(int h_res, int v_res) {
    pax_background(&fb, 0xFF64E38F);
    printf("----\r\n");
    for (int line = 0; line < num_lines; line++) {
        if (line_index[line] >= 0) {
            printf("%d: %d = %s\r\n", line, line_index[line], line_buffers[line_index[line]]);
            pax_draw_text(&fb, 0xFF2B2C3A, pax_font_sky_mono, 24, 0, 24 * line, line_buffers[line_index[line]]);
        } else {
            printf("%d: (empty)\r\n", line);
        }
    }
    printf("\r\n");
    pax_draw_rect(&fb, 0xFFFFFFFF, 0, pax_buf_get_height(&fb) - 24, pax_buf_get_width(&fb), 24);
    pax_draw_text(&fb, 0xFF000000, pax_font_sky_mono, 24, 0, pax_buf_get_height(&fb) - 24, input_buffer);
    esp_lcd_panel_draw_bitmap(lcd_panel, 0, 0, h_res, v_res, pax_buf_get_pixels(&fb));
}

void set_led_color(uint8_t led, uint32_t color) {
    led_buffer[led * 3 + 0] = (color >> 8) & 0xFF;  // G
    led_buffer[led * 3 + 1] = (color >> 16) & 0xFF; // R
    led_buffer[led * 3 + 2] = (color >> 0) & 0xFF;  // B
}

void wallpaper(int h_res, int v_res) {
    pax_insert_png_buf(&fb, wallpaper_start, wallpaper_end - wallpaper_start, 0, 0, 0);
    esp_lcd_panel_draw_bitmap(lcd_panel, 0, 0, h_res, v_res, pax_buf_get_pixels(&fb));
    set_led_color(0, 0xFC0303);
    set_led_color(1, 0xFC6F03);
    set_led_color(2, 0xF4FC03);
    set_led_color(3, 0xFC03E3);
    set_led_color(4, 0x0303FC);
    set_led_color(5, 0x03FC03);
    bsp_led_write(led_buffer, sizeof(led_buffer));
}

void app_main(void) {
    gpio_install_isr_service(0);
    ESP_ERROR_CHECK(bsp_device_initialize());
    ESP_LOGI(TAG, "Starting app...");

    bsp_led_initialize();
    bsp_led_write(led_buffer, sizeof(led_buffer));


    ESP_LOGW(TAG, "Switching radio off...\r\n");
    bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGW(TAG, "Switching radio to application mode...\r\n");
    bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_APPLICATION);
    vTaskDelay(pdMS_TO_TICKS(100));

    bsp_input_set_backlight_brightness(100);

    ESP_ERROR_CHECK(uart_driver_install(RADIO_UART, RADIO_BUFFER, RADIO_BUFFER, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_set_pin(RADIO_UART, RADIO_TX, RADIO_RX, -1, -1));
    ESP_ERROR_CHECK(uart_set_baudrate(RADIO_UART, 38400));
    ESP_ERROR_CHECK(bsp_display_get_panel(&lcd_panel));
    bsp_display_get_panel_io(&lcd_panel_io);
    size_t                       h_res, v_res;
    lcd_color_rgb_pixel_format_t colour_format;
    ESP_ERROR_CHECK(bsp_display_get_parameters(&h_res, &v_res, &colour_format));
    ESP_ERROR_CHECK(bsp_input_get_queue(&input_event_queue));

    pax_buf_init(&fb, NULL, h_res, v_res, PAX_BUF_16_565RGB);
    pax_buf_reversed(&fb, false);
    pax_buf_set_orientation(&fb, PAX_O_ROT_CW);

    init();
    blit(h_res, v_res);

    while (1) {
        int length = 0;
        ESP_ERROR_CHECK(uart_get_buffered_data_len(RADIO_UART, (size_t*)&length));
        if (length >= RADIO_BUFFER) {  // Do not use last byte of buffer to allow for
                                       // NULL terminator
            length = RADIO_BUFFER - 1;
        }
        if (length > 0) {
            uint8_t data[RADIO_BUFFER] = {0};
            length                     = uart_read_bytes(RADIO_UART, data, length, pdMS_TO_TICKS(100));
            if (length < 0) {
                ESP_LOGE(TAG, "UART READ ERROR");
            } else {
                printf("%s", data);
                add_line((char*)data);
                blit(h_res, v_res);
            }
        } else {
            bsp_input_event_t event;
            if (xQueueReceive(input_event_queue, &event, pdMS_TO_TICKS(10)) == pdTRUE) {
                switch (event.type) {
                    case INPUT_EVENT_TYPE_KEYBOARD: {
                        char ascii = event.args_keyboard.ascii;
                        if (ascii == '\b') {
                            size_t length = strlen(input_buffer);
                            if (length > 0) {
                                input_buffer[length - 1] = '\0';
                            }
                        } else if (ascii == '\r' || ascii == '\n') {
                            // Ignore
                        } else {
                            size_t length = strlen(input_buffer);
                            if (length < num_chars - 1) {
                                input_buffer[length]     = ascii;
                                input_buffer[length + 1] = '\0';
                            }
                        }
                        blit(h_res, v_res);
                        break;
                    }
                    case INPUT_EVENT_TYPE_NAVIGATION: {
                        if (event.args_navigation.state) {
                            /*if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_BACKSPACE) {
                                size_t length = strlen(input_buffer);
                                if (length > 0) {
                                    input_buffer[length - 1] = '\0';
                                }
                            }*/
                            if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
                                uart_write_bytes(RADIO_UART, input_buffer, strlen(input_buffer));
                                uart_write_bytes(RADIO_UART, "\r\n", 2);
                                add_line(input_buffer);
                                memset(input_buffer, 0, num_chars);
                            }
                            blit(h_res, v_res);
                            if (event.args_navigation.key == BSP_INPUT_NAVIGATION_KEY_F1) {
                                wallpaper(h_res, v_res);
                            }
                        }
                        break;
                    }
                    default:
                        break;
                }
            }
        }
    }
}
