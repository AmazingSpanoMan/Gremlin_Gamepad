/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/display/status_screen.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

#if IS_ENABLED(CONFIG_ZMK_WIDGET_BATTERY_STATUS)
#include <zmk/display/widgets/battery_status.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_OUTPUT_STATUS)
#include <zmk/display/widgets/output_status.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_LAYER_STATUS)
#include <zmk/display/widgets/layer_status.h>
#endif

#include "gremlin_art.h"

/* -------------------------------------------------------------------------
 * Static Canvas Buffers
 * ------------------------------------------------------------------------- */
static uint8_t boot_buf[LV_CANVAS_BUF_SIZE_INDEXED_1BIT(GREMLIN_BOOT_WIDTH, GREMLIN_BOOT_HEIGHT)];
static uint8_t sleep_buf[LV_CANVAS_BUF_SIZE_INDEXED_1BIT(GREMLIN_SLEEP_WIDTH, GREMLIN_SLEEP_HEIGHT)];
static uint8_t mascot_buf[LV_CANVAS_BUF_SIZE_INDEXED_1BIT(GREMLIN_MASCOT_WIDTH, GREMLIN_MASCOT_HEIGHT)];

/* -------------------------------------------------------------------------
 * UI Objects
 * ------------------------------------------------------------------------- */
static lv_obj_t *boot_canvas = NULL;
static lv_obj_t *sleep_canvas = NULL;
static lv_obj_t *active_cont = NULL;
static lv_obj_t *mascot_canvas = NULL;

static lv_timer_t *mascot_timer = NULL;
static lv_timer_t *idle_timer = NULL;

#if IS_ENABLED(CONFIG_ZMK_WIDGET_BATTERY_STATUS)
static struct zmk_widget_battery_status battery_status_widget;
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_OUTPUT_STATUS)
static struct zmk_widget_output_status output_status_widget;
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_LAYER_STATUS)
static struct zmk_widget_layer_status layer_status_widget;
#endif

/* -------------------------------------------------------------------------
 * Animation Sequence and State Tracking
 * ------------------------------------------------------------------------- */
static int anim_step = 0;
/* Frame sequence: 0=Idle Prowl, 1=Twitch/Tension, 2=Snarl, 3=Claw Slash */
static const uint8_t anim_sequence[] = {
    0, 0, /* Frame 0 (Prowl) for 700ms */
    1,    /* Frame 1 (Tail twitch / flex) for 350ms */
    0, 0, /* Frame 0 (Prowl) for 700ms */
    2,    /* Frame 2 (Snarl) for 350ms */
    3,    /* Frame 3 (Vicious Claw Slash!) for 400ms */
    2,    /* Frame 2 (Snarl recovery) for 300ms */
};
#define ANIM_SEQ_LEN (sizeof(anim_sequence) / sizeof(anim_sequence[0]))

static int64_t last_activity_time = 0;
static bool is_sleeping = false;
static bool boot_complete = false;

/* -------------------------------------------------------------------------
 * Timers and Callbacks
 * ------------------------------------------------------------------------- */
static void mascot_anim_cb(lv_timer_t *timer) {
    if (!mascot_canvas) {
        return;
    }
    anim_step = (anim_step + 1) % ANIM_SEQ_LEN;
    uint8_t frame_idx = anim_sequence[anim_step];
    uint8_t *dest = mascot_buf + 8; /* 8-byte palette offset */
    memcpy(dest, gremlin_mascot_frames[frame_idx], GREMLIN_MASCOT_BITMAP_SIZE);
    lv_obj_invalidate(mascot_canvas);
}

static void boot_timer_cb(lv_timer_t *timer) {
    boot_complete = true;
    last_activity_time = k_uptime_get();

    if (boot_canvas) {
        lv_obj_add_flag(boot_canvas, LV_OBJ_FLAG_HIDDEN);
    }
    if (active_cont) {
        lv_obj_clear_flag(active_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(active_cont);
    }

    /* Start idle mascot animation timer */
    mascot_timer = lv_timer_create(mascot_anim_cb, 350, NULL);
}

static void idle_check_cb(lv_timer_t *timer) {
    if (!boot_complete) {
        return;
    }
    /* Inactivity timeout: 30 seconds (30,000 ms) */
    if (!is_sleeping && (k_uptime_get() - last_activity_time > 30000)) {
        is_sleeping = true;
        if (active_cont) {
            lv_obj_add_flag(active_cont, LV_OBJ_FLAG_HIDDEN);
        }
        if (sleep_canvas) {
            lv_obj_clear_flag(sleep_canvas, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(sleep_canvas);
        }
        if (mascot_timer) {
            lv_timer_pause(mascot_timer);
        }
    }
}

/* -------------------------------------------------------------------------
 * Key Activity Listener
 * ------------------------------------------------------------------------- */
static int on_key_event(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    last_activity_time = k_uptime_get();

    if (is_sleeping) {
        is_sleeping = false;
        if (sleep_canvas) {
            lv_obj_add_flag(sleep_canvas, LV_OBJ_FLAG_HIDDEN);
        }
        if (active_cont) {
            lv_obj_clear_flag(active_cont, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(active_cont);
        }
        if (mascot_timer) {
            lv_timer_resume(mascot_timer);
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(gremlin_sleep_listener, on_key_event);
ZMK_SUBSCRIPTION(gremlin_sleep_listener, zmk_position_state_changed);

/* -------------------------------------------------------------------------
 * Main Status Screen Entry Point
 * ------------------------------------------------------------------------- */
lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_size(screen, 160, 68);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);

    /* 1. Full-Body Boot Logo Canvas (160x68) */
    boot_canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(boot_canvas, boot_buf, GREMLIN_BOOT_WIDTH, GREMLIN_BOOT_HEIGHT, LV_IMG_CF_INDEXED_1BIT);
    lv_canvas_set_palette(boot_canvas, 0, lv_color_white());
    lv_canvas_set_palette(boot_canvas, 1, lv_color_black());
    memcpy(boot_buf + 8, gremlin_boot_bitmap, GREMLIN_BOOT_BITMAP_SIZE);
    lv_obj_align(boot_canvas, LV_ALIGN_TOP_LEFT, 0, 0);

    /* 2. Full-Body Standby / Sleep Canvas (160x68) */
    sleep_canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(sleep_canvas, sleep_buf, GREMLIN_SLEEP_WIDTH, GREMLIN_SLEEP_HEIGHT, LV_IMG_CF_INDEXED_1BIT);
    lv_canvas_set_palette(sleep_canvas, 0, lv_color_white());
    lv_canvas_set_palette(sleep_canvas, 1, lv_color_black());
    memcpy(sleep_buf + 8, gremlin_sleep_bitmap, GREMLIN_SLEEP_BITMAP_SIZE);
    lv_obj_align(sleep_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(sleep_canvas, LV_OBJ_FLAG_HIDDEN);

    /* 3. Active Status Container (160x68) */
    active_cont = lv_obj_create(screen);
    lv_obj_set_size(active_cont, 160, 68);
    lv_obj_set_style_pad_all(active_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(active_cont, 0, LV_PART_MAIN);
    lv_obj_clear_flag(active_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(active_cont, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(active_cont, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(active_cont, LV_OBJ_FLAG_HIDDEN); /* Hidden until boot finishes */

    /* 3a. Full-Body Animated Mascot Canvas (64x64) on left */
    mascot_canvas = lv_canvas_create(active_cont);
    lv_canvas_set_buffer(mascot_canvas, mascot_buf, GREMLIN_MASCOT_WIDTH, GREMLIN_MASCOT_HEIGHT, LV_IMG_CF_INDEXED_1BIT);
    lv_canvas_set_palette(mascot_canvas, 0, lv_color_white());
    lv_canvas_set_palette(mascot_canvas, 1, lv_color_black());
    memcpy(mascot_buf + 8, gremlin_mascot_frames[0], GREMLIN_MASCOT_BITMAP_SIZE);
    lv_obj_align(mascot_canvas, LV_ALIGN_TOP_LEFT, 2, 2);

    /* 3b. Mascot Border Frame (66x65) */
    lv_obj_t *mascot_box = lv_obj_create(active_cont);
    lv_obj_set_size(mascot_box, 66, 65);
    lv_obj_set_style_pad_all(mascot_box, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(mascot_box, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(mascot_box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mascot_box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(mascot_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(mascot_box, LV_ALIGN_TOP_LEFT, 2, 2);

    /* 3c. Status Widgets (Original Clean 4-Row Spacing) */
#if IS_ENABLED(CONFIG_ZMK_WIDGET_LAYER_STATUS)
    zmk_widget_layer_status_init(&layer_status_widget, active_cont);
    lv_obj_align(zmk_widget_layer_status_obj(&layer_status_widget), LV_ALIGN_TOP_LEFT, 74, 8);
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_BATTERY_STATUS)
    zmk_widget_battery_status_init(&battery_status_widget, active_cont);
    lv_obj_align(zmk_widget_battery_status_obj(&battery_status_widget), LV_ALIGN_TOP_LEFT, 74, 24);
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_OUTPUT_STATUS)
    zmk_widget_output_status_init(&output_status_widget, active_cont);
    lv_obj_align(zmk_widget_output_status_obj(&output_status_widget), LV_ALIGN_TOP_LEFT, 74, 40);
#endif

    /* One-shot boot timer: 2500ms */
    lv_timer_t *b_timer = lv_timer_create(boot_timer_cb, 2500, NULL);
    lv_timer_set_repeat_count(b_timer, 1);

    /* Periodic idle check timer: every 1000ms */
    idle_timer = lv_timer_create(idle_check_cb, 1000, NULL);

    return screen;
}
