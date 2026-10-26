/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Gremlin v3 Full-Body Mascot & Display Art Bitmaps (1-bit monochrome for Sharp Memory LCD)
 * Vertical / Portrait Mode: 68 width x 160 height (flex cable facing down toward user)
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#define GREMLIN_BOOT_WIDTH 68
#define GREMLIN_BOOT_HEIGHT 160
#define GREMLIN_BOOT_BITMAP_SIZE 1440

#define GREMLIN_SLEEP_WIDTH 68
#define GREMLIN_SLEEP_HEIGHT 160
#define GREMLIN_SLEEP_BITMAP_SIZE 1440

#define GREMLIN_MASCOT_WIDTH 64
#define GREMLIN_MASCOT_HEIGHT 64
#define GREMLIN_MASCOT_BITMAP_SIZE 512
#define GREMLIN_MASCOT_FRAME_COUNT 4

extern const uint8_t gremlin_boot_bitmap[GREMLIN_BOOT_BITMAP_SIZE];
extern const uint8_t gremlin_sleep_bitmap[GREMLIN_SLEEP_BITMAP_SIZE];
extern const uint8_t gremlin_mascot_frames[GREMLIN_MASCOT_FRAME_COUNT][GREMLIN_MASCOT_BITMAP_SIZE];
