/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gremlin_art.h"

#define GREMLIN_DISPLAY_MAX_MASCOT_FRAMES 6
#define GREMLIN_DISPLAY_MAX_ANIM_SEQ 16

enum gremlin_bitmap_target {
    GREMLIN_BITMAP_MASCOT = 0,
    GREMLIN_BITMAP_BOOT = 1,
    GREMLIN_BITMAP_SLEEP = 2,
};

struct gremlin_display_config {
    uint32_t display_sleep_ms;
    bool show_layer_badge;
    bool show_battery_icon;
    bool show_battery_percent;
    bool show_output_badge;
    bool show_footer;
    bool show_mascot;
    bool show_mascot_border;
    uint32_t boot_duration_ms;
    uint32_t mascot_frame_ms;
    uint8_t mascot_frame_count;
    uint8_t anim_sequence_len;
    uint8_t anim_sequence[GREMLIN_DISPLAY_MAX_ANIM_SEQ];
};

void gremlin_display_get_config(struct gremlin_display_config *out);
int gremlin_display_set_config(const struct gremlin_display_config *cfg);

int gremlin_display_read_bitmap_chunk(enum gremlin_bitmap_target target, uint8_t slot,
                                      size_t offset, uint8_t *out, size_t req_len,
                                      size_t *actual_len_out, size_t *total_size_out,
                                      bool *is_custom_out);
int gremlin_display_write_bitmap_chunk(enum gremlin_bitmap_target target, uint8_t slot,
                                       size_t offset, size_t total_size, const uint8_t *data,
                                       size_t len);
int gremlin_display_clear_bitmap(enum gremlin_bitmap_target target, uint8_t slot);

size_t gremlin_display_custom_nvs_bytes(void);
int gremlin_display_save_settings(void);
int gremlin_display_reset_settings(void);
