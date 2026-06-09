#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    OLED_FACE_BOOT = 0,
    OLED_FACE_IDLE,
    OLED_FACE_HAPPY,
    OLED_FACE_BUSY,
    OLED_FACE_LISTEN,
    OLED_FACE_SAD,
    OLED_FACE_WIFI,
} oled_face_t;

esp_err_t oled_display_init(void);
bool oled_display_is_ready(void);
void oled_display_show(oled_face_t face, const char *title, const char *detail);
void oled_display_show_preview(oled_face_t face, const char *title, const char *preview);

/* Compatibility hook for the old OLED CLI command.
 * The ST7789 implementation ignores these arguments and reinitialises the
 * configured SPI LCD pins from mimi_config.h. */
esp_err_t oled_display_force_init(int sda, int scl, int addr_hint);
