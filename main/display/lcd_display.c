#include "display/oled_display.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

#include "mimi_config.h"

static const char *TAG = "lcd";

#if MIMI_OLED_ENABLE

#ifndef MIMI_LCD_SPI_HOST
#define MIMI_LCD_SPI_HOST SPI2_HOST
#endif

#define LCD_FLUSH_LINES 16
#define LCD_FACE_X      18
#define LCD_FACE_Y      18
#define LCD_FACE_R      32
#define LCD_TITLE_X     94
#define LCD_TITLE_Y     24
#define LCD_TITLE_SCALE 2
#define LCD_BODY_X      12
#define LCD_BODY_Y      100
#define LCD_BODY_SCALE  2
#define LCD_BODY_LINE_H 22
#define LCD_BODY_LINES  6
#define LCD_BODY_COLS   ((MIMI_LCD_WIDTH - (LCD_BODY_X * 2)) / (6 * LCD_BODY_SCALE))

#define LCD_COLOR_BG       lcd_rgb565(10, 13, 18)
#define LCD_COLOR_PANEL    lcd_rgb565(19, 28, 38)
#define LCD_COLOR_TEXT     lcd_rgb565(235, 242, 247)
#define LCD_COLOR_MUTED    lcd_rgb565(128, 148, 164)
#define LCD_COLOR_ACCENT   lcd_rgb565(64, 211, 189)
#define LCD_COLOR_WARN     lcd_rgb565(255, 108, 92)
#define LCD_COLOR_WIFI     lcd_rgb565(85, 165, 255)
#define LCD_COLOR_BUSY     lcd_rgb565(255, 199, 77)

static SemaphoreHandle_t s_lock = NULL;
static SemaphoreHandle_t s_flush_done = NULL;
static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static bool s_ready = false;
static bool s_spi_bus_ready = false;
static uint16_t *s_framebuffer = NULL;
static uint16_t *s_flush_buffer = NULL;

/* 5x7 font, columns stored LSB-top. */
static const uint8_t s_font_5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5f,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7f,0x14,0x7f,0x14},
    {0x24,0x2a,0x7f,0x2a,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1c,0x22,0x41,0x00},{0x00,0x41,0x22,0x1c,0x00},{0x14,0x08,0x3e,0x08,0x14},{0x08,0x08,0x3e,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3e,0x51,0x49,0x45,0x3e},{0x00,0x42,0x7f,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},
    {0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3c,0x4a,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1e},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3e},{0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},
    {0x7f,0x41,0x41,0x22,0x1c},{0x7f,0x49,0x49,0x49,0x41},{0x7f,0x09,0x09,0x09,0x01},{0x3e,0x41,0x49,0x49,0x7a},
    {0x7f,0x08,0x08,0x08,0x7f},{0x00,0x41,0x7f,0x41,0x00},{0x20,0x40,0x41,0x3f,0x01},{0x7f,0x08,0x14,0x22,0x41},
    {0x7f,0x40,0x40,0x40,0x40},{0x7f,0x02,0x0c,0x02,0x7f},{0x7f,0x04,0x08,0x10,0x7f},{0x3e,0x41,0x41,0x41,0x3e},
    {0x7f,0x09,0x09,0x09,0x06},{0x3e,0x41,0x51,0x21,0x5e},{0x7f,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7f,0x01,0x01},{0x3f,0x40,0x40,0x40,0x3f},{0x1f,0x20,0x40,0x20,0x1f},{0x3f,0x40,0x38,0x40,0x3f},
    {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7f,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7f,0x00},{0x04,0x02,0x01,0x02,0x04},{0x80,0x80,0x80,0x80,0x80},
    {0x00,0x03,0x05,0x00,0x00},{0x20,0x54,0x54,0x54,0x78},{0x7f,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7f},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7e,0x09,0x01,0x02},{0x0c,0x52,0x52,0x52,0x3e},
    {0x7f,0x08,0x04,0x04,0x78},{0x00,0x44,0x7d,0x40,0x00},{0x20,0x40,0x44,0x3d,0x00},{0x7f,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7f,0x40,0x00},{0x7c,0x04,0x18,0x04,0x78},{0x7c,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7c,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7c},{0x7c,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3f,0x44,0x40,0x20},{0x3c,0x40,0x40,0x20,0x7c},{0x1c,0x20,0x40,0x20,0x1c},{0x3c,0x40,0x30,0x40,0x3c},
    {0x44,0x28,0x10,0x28,0x44},{0x0c,0x50,0x50,0x50,0x3c},{0x44,0x64,0x54,0x4c,0x44},{0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7f,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x08,0x04,0x08,0x10,0x08}
};

static uint16_t lcd_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return (uint16_t)((c << 8) | (c >> 8));
}

static bool lcd_color_done_cb(esp_lcd_panel_io_handle_t panel_io,
                              esp_lcd_panel_io_event_data_t *edata,
                              void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    BaseType_t task_woken = pdFALSE;
    if (user_ctx) {
        xSemaphoreGiveFromISR((SemaphoreHandle_t)user_ctx, &task_woken);
    }
    return task_woken == pdTRUE;
}

static void lcd_set_pixel(int x, int y, uint16_t color)
{
    if (!s_framebuffer || x < 0 || x >= MIMI_LCD_WIDTH || y < 0 || y >= MIMI_LCD_HEIGHT) {
        return;
    }
    s_framebuffer[y * MIMI_LCD_WIDTH + x] = color;
}

static void lcd_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_framebuffer || w <= 0 || h <= 0) {
        return;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > MIMI_LCD_WIDTH) {
        w = MIMI_LCD_WIDTH - x;
    }
    if (y + h > MIMI_LCD_HEIGHT) {
        h = MIMI_LCD_HEIGHT - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    for (int row = 0; row < h; ++row) {
        uint16_t *dst = &s_framebuffer[(y + row) * MIMI_LCD_WIDTH + x];
        for (int col = 0; col < w; ++col) {
            dst[col] = color;
        }
    }
}

static void lcd_clear(void)
{
    lcd_fill_rect(0, 0, MIMI_LCD_WIDTH, MIMI_LCD_HEIGHT, LCD_COLOR_BG);
}

static void lcd_draw_hline(int x, int y, int len, uint16_t color)
{
    lcd_fill_rect(x, y, len, 1, color);
}

static void lcd_draw_vline(int x, int y, int len, uint16_t color)
{
    lcd_fill_rect(x, y, 1, len, color);
}

static void lcd_draw_line(int x0, int y0, int x1, int y1, uint16_t color)
{
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -((y1 > y0) ? (y1 - y0) : (y0 - y1));
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (1) {
        lcd_set_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = err << 1;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void lcd_draw_circle(int cx, int cy, int r, uint16_t color)
{
    int x = r;
    int y = 0;
    int err = 1 - x;

    while (x >= y) {
        lcd_set_pixel(cx + x, cy + y, color);
        lcd_set_pixel(cx + y, cy + x, color);
        lcd_set_pixel(cx - y, cy + x, color);
        lcd_set_pixel(cx - x, cy + y, color);
        lcd_set_pixel(cx - x, cy - y, color);
        lcd_set_pixel(cx - y, cy - x, color);
        lcd_set_pixel(cx + y, cy - x, color);
        lcd_set_pixel(cx + x, cy - y, color);
        ++y;
        if (err < 0) {
            err += (y << 1) + 1;
        } else {
            --x;
            err += ((y - x) << 1) + 1;
        }
    }
}

static void lcd_draw_char(int x, int y, char c, int scale, uint16_t color)
{
    if (c < 32 || c > 126) {
        c = '?';
    }
    const uint8_t *glyph = s_font_5x7[(int)c - 32];
    for (int col = 0; col < 5; ++col) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; ++row) {
            if (bits & (1u << row)) {
                lcd_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

static void lcd_draw_text(int x, int y, const char *text, int max_chars, int scale, uint16_t color)
{
    if (!text) {
        return;
    }
    for (int i = 0; text[i] && i < max_chars; ++i) {
        lcd_draw_char(x + i * 6 * scale, y, text[i], scale, color);
    }
}

static void lcd_prepare_ascii_line(char *dst, size_t dst_size, const char *src, bool uppercase)
{
    size_t out = 0;

    if (!dst || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }

    while (*src && out + 1 < dst_size) {
        unsigned char ch = (unsigned char)*src++;
        if (ch >= 32 && ch <= 126) {
            if (uppercase) {
                ch = (unsigned char)toupper(ch);
            }
            dst[out++] = (char)ch;
        } else if (ch == '\n' || ch == '\r' || ch == '\t') {
            if (out > 0 && dst[out - 1] != ' ') {
                dst[out++] = ' ';
            }
        } else if (ch < 0x80) {
            dst[out++] = '?';
        }
    }

    while (out > 0 && dst[out - 1] == ' ') {
        --out;
    }
    dst[out] = '\0';
}

static void lcd_wrap_ascii_lines(char lines[][LCD_BODY_COLS + 1], size_t line_count, const char *src)
{
    char ascii[LCD_BODY_COLS * LCD_BODY_LINES + 24];
    size_t src_idx = 0;

    memset(lines, 0, line_count * (LCD_BODY_COLS + 1));
    lcd_prepare_ascii_line(ascii, sizeof(ascii), src, false);

    size_t len = strlen(ascii);
    if (len == 0) {
        strlcpy(lines[0], "waiting...", LCD_BODY_COLS + 1);
        return;
    }

    for (size_t line = 0; line < line_count && src_idx < len; ++line) {
        size_t remaining = len - src_idx;
        size_t chunk = remaining > LCD_BODY_COLS ? LCD_BODY_COLS : remaining;

        memcpy(lines[line], ascii + src_idx, chunk);
        lines[line][chunk] = '\0';
        src_idx += chunk;
        while (ascii[src_idx] == ' ') {
            ++src_idx;
        }
    }
}

static uint16_t lcd_face_color(oled_face_t face)
{
    switch (face) {
        case OLED_FACE_SAD:
            return LCD_COLOR_WARN;
        case OLED_FACE_BUSY:
        case OLED_FACE_LISTEN:
            return LCD_COLOR_BUSY;
        case OLED_FACE_WIFI:
            return LCD_COLOR_WIFI;
        default:
            return LCD_COLOR_ACCENT;
    }
}

static void lcd_draw_face(oled_face_t face)
{
    const int cx = LCD_FACE_X + LCD_FACE_R;
    const int cy = LCD_FACE_Y + LCD_FACE_R;
    const int left_eye_x = cx - 14;
    const int right_eye_x = cx + 10;
    const int eye_y = cy - 12;
    uint16_t accent = lcd_face_color(face);

    for (int r = LCD_FACE_R; r > LCD_FACE_R - 3; --r) {
        lcd_draw_circle(cx, cy, r, accent);
    }
    lcd_fill_rect(left_eye_x, eye_y, 7, 7, LCD_COLOR_TEXT);
    lcd_fill_rect(right_eye_x, eye_y, 7, 7, LCD_COLOR_TEXT);

    switch (face) {
        case OLED_FACE_BOOT:
            lcd_draw_hline(cx - 14, cy + 13, 28, LCD_COLOR_TEXT);
            lcd_draw_vline(cx, cy + 4, 19, LCD_COLOR_TEXT);
            break;
        case OLED_FACE_IDLE:
        case OLED_FACE_WIFI:
            lcd_draw_line(cx - 18, cy + 12, cx - 8, cy + 22, LCD_COLOR_TEXT);
            lcd_draw_hline(cx - 8, cy + 22, 18, LCD_COLOR_TEXT);
            lcd_draw_line(cx + 10, cy + 22, cx + 19, cy + 12, LCD_COLOR_TEXT);
            break;
        case OLED_FACE_HAPPY:
            lcd_fill_rect(left_eye_x - 2, eye_y, 11, 4, LCD_COLOR_TEXT);
            lcd_fill_rect(right_eye_x - 2, eye_y, 11, 4, LCD_COLOR_TEXT);
            lcd_draw_line(cx - 20, cy + 10, cx - 9, cy + 24, LCD_COLOR_TEXT);
            lcd_draw_hline(cx - 9, cy + 24, 22, LCD_COLOR_TEXT);
            lcd_draw_line(cx + 13, cy + 24, cx + 21, cy + 10, LCD_COLOR_TEXT);
            break;
        case OLED_FACE_BUSY:
            lcd_draw_line(left_eye_x - 2, eye_y - 5, left_eye_x + 8, eye_y, LCD_COLOR_TEXT);
            lcd_draw_line(right_eye_x - 2, eye_y, right_eye_x + 8, eye_y - 5, LCD_COLOR_TEXT);
            lcd_draw_hline(cx - 15, cy + 20, 30, LCD_COLOR_TEXT);
            break;
        case OLED_FACE_LISTEN:
            lcd_fill_rect(cx - 5, cy + 10, 10, 10, LCD_COLOR_TEXT);
            lcd_draw_line(cx - 28, cy - 2, cx - 34, cy + 5, accent);
            lcd_draw_line(cx + 28, cy - 2, cx + 34, cy + 5, accent);
            break;
        case OLED_FACE_SAD:
            lcd_draw_line(left_eye_x - 1, eye_y - 1, left_eye_x + 8, eye_y + 8, LCD_COLOR_TEXT);
            lcd_draw_line(left_eye_x + 8, eye_y - 1, left_eye_x - 1, eye_y + 8, LCD_COLOR_TEXT);
            lcd_draw_line(right_eye_x - 1, eye_y - 1, right_eye_x + 8, eye_y + 8, LCD_COLOR_TEXT);
            lcd_draw_line(right_eye_x + 8, eye_y - 1, right_eye_x - 1, eye_y + 8, LCD_COLOR_TEXT);
            lcd_draw_line(cx - 18, cy + 24, cx - 8, cy + 14, LCD_COLOR_TEXT);
            lcd_draw_hline(cx - 8, cy + 14, 18, LCD_COLOR_TEXT);
            lcd_draw_line(cx + 10, cy + 14, cx + 20, cy + 24, LCD_COLOR_TEXT);
            break;
    }
}

static esp_err_t lcd_send_param(int cmd, const uint8_t *data, size_t len)
{
    return esp_lcd_panel_io_tx_param(s_io, cmd, data, len);
}

static esp_err_t lcd_send_st7789_init_sequence(void)
{
    static const uint8_t madctl[] = { 0x00 };
    static const uint8_t colmod[] = { 0x05 };
    static const uint8_t porch[] = { 0x0C, 0x0C, 0x00, 0x33, 0x33 };
    static const uint8_t gate[] = { 0x35 };
    static const uint8_t vcom[] = { 0x19 };
    static const uint8_t lcm[] = { 0x2C };
    static const uint8_t vdv_vrh[] = { 0x01 };
    static const uint8_t vrh[] = { 0x12 };
    static const uint8_t vdv[] = { 0x20 };
    static const uint8_t frame_rate[] = { 0x0F };
    static const uint8_t power[] = { 0xA4, 0xA1 };
    static const uint8_t gamma_pos[] = {
        0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F,
        0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23
    };
    static const uint8_t gamma_neg[] = {
        0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F,
        0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23
    };

    ESP_RETURN_ON_ERROR(lcd_send_param(0x36, madctl, sizeof(madctl)), TAG, "MADCTL failed");
    ESP_RETURN_ON_ERROR(lcd_send_param(0x3A, colmod, sizeof(colmod)), TAG, "COLMOD failed");
    ESP_RETURN_ON_ERROR(lcd_send_param(0xB2, porch, sizeof(porch)), TAG, "PORCTRL failed");
    ESP_RETURN_ON_ERROR(lcd_send_param(0xB7, gate, sizeof(gate)), TAG, "GCTRL failed");
    ESP_RETURN_ON_ERROR(lcd_send_param(0xBB, vcom, sizeof(vcom)), TAG, "VCOMS failed");
    ESP_RETURN_ON_ERROR(lcd_send_param(0xC0, lcm, sizeof(lcm)), TAG, "LCMCTRL failed");
    ESP_RETURN_ON_ERROR(lcd_send_param(0xC2, vdv_vrh, sizeof(vdv_vrh)), TAG, "VDVVRHEN failed");
    ESP_RETURN_ON_ERROR(lcd_send_param(0xC3, vrh, sizeof(vrh)), TAG, "VRHS failed");
    ESP_RETURN_ON_ERROR(lcd_send_param(0xC4, vdv, sizeof(vdv)), TAG, "VDVS failed");
    ESP_RETURN_ON_ERROR(lcd_send_param(0xC6, frame_rate, sizeof(frame_rate)), TAG, "FRCTRL2 failed");
    ESP_RETURN_ON_ERROR(lcd_send_param(0xD0, power, sizeof(power)), TAG, "PWCTRL1 failed");
    ESP_RETURN_ON_ERROR(lcd_send_param(0xE0, gamma_pos, sizeof(gamma_pos)), TAG, "PVGAMCTRL failed");
    ESP_RETURN_ON_ERROR(lcd_send_param(0xE1, gamma_neg, sizeof(gamma_neg)), TAG, "NVGAMCTRL failed");
    ESP_RETURN_ON_ERROR(lcd_send_param(0x21, NULL, 0), TAG, "INVON failed");
    ESP_RETURN_ON_ERROR(lcd_send_param(0x11, NULL, 0), TAG, "SLPOUT failed");
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_RETURN_ON_ERROR(lcd_send_param(0x29, NULL, 0), TAG, "DISPON failed");
    return ESP_OK;
}

static esp_err_t lcd_flush_locked(void)
{
    if (!s_ready || !s_panel || !s_framebuffer || !s_flush_buffer) {
        return ESP_ERR_INVALID_STATE;
    }

    for (int y = 0; y < MIMI_LCD_HEIGHT; y += LCD_FLUSH_LINES) {
        int lines = MIMI_LCD_HEIGHT - y;
        if (lines > LCD_FLUSH_LINES) {
            lines = LCD_FLUSH_LINES;
        }
        size_t bytes = (size_t)MIMI_LCD_WIDTH * (size_t)lines * sizeof(uint16_t);
        memcpy(s_flush_buffer, &s_framebuffer[y * MIMI_LCD_WIDTH], bytes);

        if (s_flush_done) {
            xSemaphoreTake(s_flush_done, 0);
        }
        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_draw_bitmap(s_panel, 0, y, MIMI_LCD_WIDTH, y + lines, s_flush_buffer),
            TAG, "draw bitmap failed");
        if (s_flush_done && xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

static void lcd_release_hw(void)
{
    s_ready = false;
    if (s_panel) {
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
    }
    if (s_io) {
        esp_lcd_panel_io_del(s_io);
        s_io = NULL;
    }
    if (s_spi_bus_ready) {
        spi_bus_free(MIMI_LCD_SPI_HOST);
        s_spi_bus_ready = false;
    }
    if (s_framebuffer) {
        heap_caps_free(s_framebuffer);
        s_framebuffer = NULL;
    }
    if (s_flush_buffer) {
        heap_caps_free(s_flush_buffer);
        s_flush_buffer = NULL;
    }
}

static esp_err_t lcd_alloc_buffers(void)
{
    size_t fb_pixels = (size_t)MIMI_LCD_WIDTH * (size_t)MIMI_LCD_HEIGHT;
    size_t flush_pixels = (size_t)MIMI_LCD_WIDTH * LCD_FLUSH_LINES;

    s_framebuffer = heap_caps_calloc(fb_pixels, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_framebuffer) {
        s_framebuffer = heap_caps_calloc(fb_pixels, sizeof(uint16_t), MALLOC_CAP_8BIT);
    }
    if (!s_framebuffer) {
        return ESP_ERR_NO_MEM;
    }

    s_flush_buffer = heap_caps_malloc(flush_pixels * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_flush_buffer) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t lcd_backlight_config(bool on)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << MIMI_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "backlight gpio config failed");
    return gpio_set_level((gpio_num_t)MIMI_LCD_BL, on ? 1 : 0);
}

static esp_err_t lcd_init_hw(void)
{
    ESP_RETURN_ON_ERROR(lcd_alloc_buffers(), TAG, "display buffer allocation failed");
    ESP_RETURN_ON_ERROR(lcd_backlight_config(false), TAG, "backlight off failed");
    vTaskDelay(pdMS_TO_TICKS(40));

    spi_bus_config_t bus_cfg = {
        .sclk_io_num = MIMI_LCD_SPI_SCLK,
        .mosi_io_num = MIMI_LCD_SPI_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = MIMI_LCD_WIDTH * LCD_FLUSH_LINES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(MIMI_LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "SPI bus init failed");
    s_spi_bus_ready = true;

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = MIMI_LCD_CS,
        .dc_gpio_num = MIMI_LCD_DC,
        .spi_mode = MIMI_LCD_SPI_MODE,
        .pclk_hz = MIMI_LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 1,
        .on_color_trans_done = lcd_color_done_cb,
        .user_ctx = s_flush_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)MIMI_LCD_SPI_HOST,
                                                 &io_cfg, &s_io),
                        TAG, "panel IO init failed");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = MIMI_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel),
                        TAG, "ST7789 panel create failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(lcd_send_st7789_init_sequence(), TAG, "MSP1541 init sequence failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, MIMI_LCD_X_GAP, MIMI_LCD_Y_GAP),
                        TAG, "panel gap config failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, MIMI_LCD_SWAP_XY),
                        TAG, "panel swap config failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, MIMI_LCD_MIRROR_X, MIMI_LCD_MIRROR_Y),
                        TAG, "panel mirror config failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "panel on failed");
    ESP_RETURN_ON_ERROR(lcd_backlight_config(true), TAG, "backlight on failed");

    ESP_LOGI(TAG, "ST7789 initialised: clk=%d mosi=%d rst=%d dc=%d cs=%d bl=%d",
             MIMI_LCD_SPI_SCLK, MIMI_LCD_SPI_MOSI, MIMI_LCD_RST,
             MIMI_LCD_DC, MIMI_LCD_CS, MIMI_LCD_BL);
    return ESP_OK;
}

esp_err_t oled_display_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_flush_done) {
        s_flush_done = xSemaphoreCreateBinary();
        if (!s_flush_done) {
            return ESP_ERR_NO_MEM;
        }
    }

    lcd_release_hw();
    esp_err_t err = lcd_init_hw();
    if (err != ESP_OK) {
        lcd_release_hw();
        ESP_LOGE(TAG, "ST7789 init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    oled_display_show(OLED_FACE_BOOT, "BOOT", "display online");
    return ESP_OK;
}

bool oled_display_is_ready(void)
{
    return s_ready;
}

void oled_display_show(oled_face_t face, const char *title, const char *detail)
{
    char title_buf[16];
    char detail_lines[LCD_BODY_LINES][LCD_BODY_COLS + 1];

    if (!s_ready || !s_lock) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    lcd_prepare_ascii_line(title_buf, sizeof(title_buf), title, true);
    lcd_wrap_ascii_lines(detail_lines, LCD_BODY_LINES, detail);

    lcd_clear();
    lcd_fill_rect(0, 0, MIMI_LCD_WIDTH, 88, LCD_COLOR_PANEL);
    lcd_draw_face(face);
    lcd_draw_text(LCD_TITLE_X, LCD_TITLE_Y, title_buf[0] ? title_buf : "MIMI",
                  11, LCD_TITLE_SCALE, LCD_COLOR_TEXT);
    lcd_draw_hline(LCD_TITLE_X, LCD_TITLE_Y + 22, 128, LCD_COLOR_MUTED);
    lcd_draw_text(LCD_TITLE_X, LCD_TITLE_Y + 34, "MIMICLAW", 8, 1, LCD_COLOR_MUTED);

    for (size_t i = 0; i < LCD_BODY_LINES; ++i) {
        lcd_draw_text(LCD_BODY_X, LCD_BODY_Y + (int)i * LCD_BODY_LINE_H,
                      detail_lines[i], LCD_BODY_COLS, LCD_BODY_SCALE, LCD_COLOR_TEXT);
    }
    if (lcd_flush_locked() != ESP_OK) {
        ESP_LOGW(TAG, "display flush failed");
    }

    xSemaphoreGive(s_lock);
}

void oled_display_show_preview(oled_face_t face, const char *title, const char *preview)
{
    oled_display_show(face, title, preview);
}

esp_err_t oled_display_force_init(int sda, int scl, int addr_hint)
{
    (void)sda;
    (void)scl;
    (void)addr_hint;
    return oled_display_init();
}

#else

esp_err_t oled_display_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool oled_display_is_ready(void)
{
    return false;
}

void oled_display_show(oled_face_t face, const char *title, const char *detail)
{
    (void)face;
    (void)title;
    (void)detail;
}

void oled_display_show_preview(oled_face_t face, const char *title, const char *preview)
{
    (void)face;
    (void)title;
    (void)preview;
}

esp_err_t oled_display_force_init(int sda, int scl, int addr_hint)
{
    (void)sda;
    (void)scl;
    (void)addr_hint;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
