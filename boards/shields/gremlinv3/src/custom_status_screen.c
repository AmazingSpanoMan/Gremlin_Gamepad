/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <stdio.h>
#include <string.h>
#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/display/status_screen.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/keymap.h>
#include <zmk/battery.h>
#include <zmk/usb.h>
#include <zmk/endpoints.h>
#include <zmk/ble.h>

#include "gremlin_art.h"

/* -------------------------------------------------------------------------
 * Layout & Sizing Constants
 * ------------------------------------------------------------------------- */
#define STATUS_PHYS_W 88
#define STATUS_PHYS_H 68
#define STATUS_STRIDE 11 /* (88 + 7) / 8 */

#define USER_CANVAS_W 68
#define USER_CANVAS_H 88
#define USER_STRIDE 9    /* (68 + 7) / 8 */

/* -------------------------------------------------------------------------
 * Static Canvas Buffers (All 1-bit monochrome indexed)
 * ------------------------------------------------------------------------- */
static uint8_t boot_buf[LV_CANVAS_BUF_SIZE_INDEXED_1BIT(GREMLIN_BOOT_WIDTH, GREMLIN_BOOT_HEIGHT)];
static uint8_t sleep_buf[LV_CANVAS_BUF_SIZE_INDEXED_1BIT(GREMLIN_SLEEP_WIDTH, GREMLIN_SLEEP_HEIGHT)];
static uint8_t mascot_buf[LV_CANVAS_BUF_SIZE_INDEXED_1BIT(GREMLIN_MASCOT_WIDTH, GREMLIN_MASCOT_HEIGHT)];

static uint8_t status_phys_buf[LV_CANVAS_BUF_SIZE_INDEXED_1BIT(STATUS_PHYS_W, STATUS_PHYS_H)];
static uint8_t user_virt_buf[LV_CANVAS_BUF_SIZE_INDEXED_1BIT(USER_CANVAS_W, USER_CANVAS_H)];

/* -------------------------------------------------------------------------
 * UI Objects
 * ------------------------------------------------------------------------- */
static lv_obj_t *boot_canvas = NULL;
static lv_obj_t *sleep_canvas = NULL;
static lv_obj_t *active_cont = NULL;
static lv_obj_t *mascot_canvas = NULL;
static lv_obj_t *status_canvas = NULL;
static lv_obj_t *user_canvas = NULL;

static lv_timer_t *mascot_timer = NULL;
static lv_timer_t *idle_timer = NULL;

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

/* Live state */
static zmk_keymap_layer_index_t current_layer_index = 0;
static const char *current_layer_label = NULL;
static uint8_t current_battery_level = 100;
static bool is_usb_charging = false;
static bool is_usb_output = true;
static int active_ble_profile = 0;

/* -------------------------------------------------------------------------
 * Draw Status Content (1:1 Pixel-Perfect Rotation into Physical Screen)
 * ------------------------------------------------------------------------- */
static void draw_status_content(void) {
    if (!user_canvas || !status_canvas) {
        return;
    }

    /* 1. Clear Virtual User Canvas (68 wide x 88 tall, White Background) */
    lv_canvas_fill_bg(user_canvas, lv_color_white(), LV_OPA_COVER);

    /* 2. Layer Name Badge (Solid High-Contrast Inverted Pill / Badge) */
    lv_draw_rect_dsc_t rect_badge;
    lv_draw_rect_dsc_init(&rect_badge);
    rect_badge.bg_color = lv_color_black();
    rect_badge.radius = 3;
    lv_canvas_draw_rect(user_canvas, 3, 3, 62, 19, &rect_badge);

    char layer_text[16] = {};
    if (current_layer_label == NULL || strlen(current_layer_label) == 0) {
        snprintf(layer_text, sizeof(layer_text), "LAYER %d", current_layer_index);
    } else {
        snprintf(layer_text, sizeof(layer_text), "%s", current_layer_label);
    }
    for (int i = 0; layer_text[i]; i++) {
        if (layer_text[i] >= 'a' && layer_text[i] <= 'z') {
            layer_text[i] -= 32;
        }
    }

    lv_draw_label_dsc_t lbl_white;
    lv_draw_label_dsc_init(&lbl_white);
    lbl_white.color = lv_color_white();
    lbl_white.font = &lv_font_montserrat_14;
    lbl_white.align = LV_TEXT_ALIGN_CENTER;
    lv_canvas_draw_text(user_canvas, 3, 5, 62, &lbl_white, layer_text);

    /* 3. Battery Status (Icon + Percentage Text) */
    lv_draw_rect_dsc_t rect_blk, rect_wht;
    lv_draw_rect_dsc_init(&rect_blk);
    rect_blk.bg_color = lv_color_black();
    lv_draw_rect_dsc_init(&rect_wht);
    rect_wht.bg_color = lv_color_white();

    /* Battery outline: 22 wide x 11 tall */
    lv_canvas_draw_rect(user_canvas, 5, 29, 22, 11, &rect_blk);
    lv_canvas_draw_rect(user_canvas, 6, 30, 20, 9, &rect_wht);
    /* Terminal: 2 wide x 5 tall */
    lv_canvas_draw_rect(user_canvas, 27, 32, 2, 5, &rect_blk);
    /* Fill level inside battery */
    int fill_w = (current_battery_level * 18) / 100;
    if (fill_w > 18) fill_w = 18;
    if (fill_w > 0) {
        lv_canvas_draw_rect(user_canvas, 7, 31, fill_w, 7, &rect_blk);
    }

    /* Battery percentage */
    char bat_text[12] = {};
    if (is_usb_charging) {
        snprintf(bat_text, sizeof(bat_text), "%d%%+", current_battery_level);
    } else {
        snprintf(bat_text, sizeof(bat_text), "%d%%", current_battery_level);
    }

    lv_draw_label_dsc_t lbl_black;
    lv_draw_label_dsc_init(&lbl_black);
    lbl_black.color = lv_color_black();
    lbl_black.font = &lv_font_montserrat_14;
    lbl_black.align = LV_TEXT_ALIGN_LEFT;
    lv_canvas_draw_text(user_canvas, 32, 27, 34, &lbl_black, bat_text);

    /* 4. Output Status Badge (USB or BLE Profile) */
    lv_canvas_draw_rect(user_canvas, 3, 46, 62, 19, &rect_badge);
    char out_text[16] = {};
    if (is_usb_output) {
        snprintf(out_text, sizeof(out_text), "USB");
    } else {
        snprintf(out_text, sizeof(out_text), "BLE %d", active_ble_profile + 1);
    }
    lv_canvas_draw_text(user_canvas, 3, 48, 62, &lbl_white, out_text);

    /* 5. Footer Divider & Branding */
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = lv_color_black();
    line_dsc.width = 1;
    lv_point_t p_div[2] = {{6, 70}, {62, 70}};
    lv_canvas_draw_line(user_canvas, p_div, 2, &line_dsc);

    lbl_black.align = LV_TEXT_ALIGN_CENTER;
    lbl_black.font = &lv_font_montserrat_12;
    lv_canvas_draw_text(user_canvas, 0, 73, 68, &lbl_black, "GREMLIN v3");

    /* 6. Exact 1:1 Pixel Transposition (Rotate 90 deg clockwise into physical 88x68 canvas) */
    memset(status_phys_buf + 8, 0, STATUS_STRIDE * STATUS_PHYS_H);

    const uint8_t *u_src = user_virt_buf + 8;
    uint8_t *p_dst = status_phys_buf + 8;

    for (int uy = 0; uy < USER_CANVAS_H; uy++) {
        int px = 87 - uy;
        int px_byte = px >> 3;
        uint8_t px_bit = 1 << (7 - (px & 7));
        const uint8_t *u_row = u_src + (uy * USER_STRIDE);

        for (int ux = 0; ux < USER_CANVAS_W; ux++) {
            int py = ux;
            uint8_t bit = (u_row[ux >> 3] >> (7 - (ux & 7))) & 1;
            if (bit) {
                p_dst[py * STATUS_STRIDE + px_byte] |= px_bit;
            }
        }
    }

    lv_obj_invalidate(status_canvas);
}

/* -------------------------------------------------------------------------
 * ZMK Widget Event Listeners
 * ------------------------------------------------------------------------- */
struct layer_status_state {
    zmk_keymap_layer_index_t index;
    const char *label;
};

static void layer_status_update_cb(struct layer_status_state state) {
    current_layer_index = state.index;
    current_layer_label = state.label;
    draw_status_content();
}

static struct layer_status_state layer_status_get_state(const zmk_event_t *eh) {
    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();
    return (struct layer_status_state){
        .index = index,
        .label = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index)),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(gremlin_layer_status, struct layer_status_state,
                            layer_status_update_cb, layer_status_get_state)
ZMK_SUBSCRIPTION(gremlin_layer_status, zmk_layer_state_changed);

struct battery_status_state {
    uint8_t level;
    bool usb_present;
};

static void battery_status_update_cb(struct battery_status_state state) {
    current_battery_level = state.level;
    is_usb_charging = state.usb_present;
    draw_status_content();
}

static struct battery_status_state battery_status_get_state(const zmk_event_t *eh) {
    return (struct battery_status_state){
        .level = zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#else
        .usb_present = false,
#endif
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(gremlin_battery_status, struct battery_status_state,
                            battery_status_update_cb, battery_status_get_state)
ZMK_SUBSCRIPTION(gremlin_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(gremlin_battery_status, zmk_usb_conn_state_changed);
#endif

struct output_status_state {
    struct zmk_endpoint_instance selected_endpoint;
    int active_profile_index;
};

static void output_status_update_cb(struct output_status_state state) {
    is_usb_output = (state.selected_endpoint.transport == ZMK_TRANSPORT_USB);
    active_ble_profile = state.active_profile_index;
    draw_status_content();
}

static struct output_status_state output_status_get_state(const zmk_event_t *eh) {
    return (struct output_status_state){
        .selected_endpoint = zmk_endpoints_selected(),
        .active_profile_index = zmk_ble_active_profile_index(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(gremlin_output_status, struct output_status_state,
                            output_status_update_cb, output_status_get_state)
ZMK_SUBSCRIPTION(gremlin_output_status, zmk_endpoint_changed);
#if defined(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(gremlin_output_status, zmk_ble_active_profile_changed);
#endif

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

    draw_status_content();

    /* Start idle mascot animation timer (every 350ms) */
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
        draw_status_content();
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
    if (!screen) {
        return NULL;
    }
    lv_obj_set_size(screen, 160, 68);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);

    /* 1. Full-Body Boot Logo Canvas (160x68 Native Landscape) */
    boot_canvas = lv_canvas_create(screen);
    if (boot_canvas) {
        lv_canvas_set_buffer(boot_canvas, boot_buf, GREMLIN_BOOT_WIDTH, GREMLIN_BOOT_HEIGHT, LV_IMG_CF_INDEXED_1BIT);
        lv_canvas_set_palette(boot_canvas, 0, lv_color_white());
        lv_canvas_set_palette(boot_canvas, 1, lv_color_black());
        memcpy(boot_buf + 8, gremlin_boot_bitmap, GREMLIN_BOOT_BITMAP_SIZE);
        lv_obj_align(boot_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    }

    /* 2. Full-Body Standby / Sleep Canvas (160x68 Native Landscape) */
    sleep_canvas = lv_canvas_create(screen);
    if (sleep_canvas) {
        lv_canvas_set_buffer(sleep_canvas, sleep_buf, GREMLIN_SLEEP_WIDTH, GREMLIN_SLEEP_HEIGHT, LV_IMG_CF_INDEXED_1BIT);
        lv_canvas_set_palette(sleep_canvas, 0, lv_color_white());
        lv_canvas_set_palette(sleep_canvas, 1, lv_color_black());
        memcpy(sleep_buf + 8, gremlin_sleep_bitmap, GREMLIN_SLEEP_BITMAP_SIZE);
        lv_obj_align(sleep_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_add_flag(sleep_canvas, LV_OBJ_FLAG_HIDDEN);
    }

    /* 3. Active Status Container (160x68 Physical) */
    active_cont = lv_obj_create(screen);
    if (active_cont) {
        lv_obj_set_size(active_cont, 160, 68);
        lv_obj_set_style_pad_all(active_cont, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(active_cont, 0, LV_PART_MAIN);
        lv_obj_clear_flag(active_cont, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(active_cont, lv_color_white(), LV_PART_MAIN);
        lv_obj_align(active_cont, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_add_flag(active_cont, LV_OBJ_FLAG_HIDDEN); /* Hidden until boot finishes */

        /* 3a. Mascot Border Frame (66x66) at User Top */
        lv_obj_t *mascot_box = lv_obj_create(active_cont);
        if (mascot_box) {
            lv_obj_set_size(mascot_box, 66, 66);
            lv_obj_set_style_pad_all(mascot_box, 0, LV_PART_MAIN);
            lv_obj_set_style_border_width(mascot_box, 1, LV_PART_MAIN);
            lv_obj_set_style_border_color(mascot_box, lv_color_black(), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(mascot_box, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_clear_flag(mascot_box, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_align(mascot_box, LV_ALIGN_TOP_LEFT, 93, 1);
        }

        /* 3b. Mascot Canvas (64x64) inside Border Frame */
        mascot_canvas = lv_canvas_create(active_cont);
        if (mascot_canvas) {
            lv_canvas_set_buffer(mascot_canvas, mascot_buf, GREMLIN_MASCOT_WIDTH, GREMLIN_MASCOT_HEIGHT, LV_IMG_CF_INDEXED_1BIT);
            lv_canvas_set_palette(mascot_canvas, 0, lv_color_white());
            lv_canvas_set_palette(mascot_canvas, 1, lv_color_black());
            memcpy(mascot_buf + 8, gremlin_mascot_frames[0], GREMLIN_MASCOT_BITMAP_SIZE);
            lv_obj_align(mascot_canvas, LV_ALIGN_TOP_LEFT, 94, 2);
        }

        /* 3c. Divider Line between Mascot and Status (in vertical orientation) */
        lv_obj_t *div1 = lv_obj_create(active_cont);
        if (div1) {
            lv_obj_set_size(div1, 1, 64);
            lv_obj_set_style_bg_color(div1, lv_color_black(), LV_PART_MAIN);
            lv_obj_set_style_border_width(div1, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(div1, 0, LV_PART_MAIN);
            lv_obj_align(div1, LV_ALIGN_TOP_LEFT, 91, 2);
        }

        /* 3d. Status Canvas (88x68 1-bit Physical) */
        status_canvas = lv_canvas_create(active_cont);
        if (status_canvas) {
            lv_canvas_set_buffer(status_canvas, status_phys_buf, STATUS_PHYS_W, STATUS_PHYS_H, LV_IMG_CF_INDEXED_1BIT);
            lv_canvas_set_palette(status_canvas, 0, lv_color_white());
            lv_canvas_set_palette(status_canvas, 1, lv_color_black());
            lv_obj_align(status_canvas, LV_ALIGN_TOP_LEFT, 2, 0);
        }

        /* 3e. Off-Screen Virtual User Canvas (68x88 for Razor-Sharp Layout) */
        user_canvas = lv_canvas_create(active_cont);
        if (user_canvas) {
            lv_canvas_set_buffer(user_canvas, user_virt_buf, USER_CANVAS_W, USER_CANVAS_H, LV_IMG_CF_INDEXED_1BIT);
            lv_canvas_set_palette(user_canvas, 0, lv_color_white());
            lv_canvas_set_palette(user_canvas, 1, lv_color_black());
            lv_obj_set_pos(user_canvas, -200, -200);
        }
    }

    /* Initialize ZMK widget state subscriptions */
    gremlin_layer_status_init();
    gremlin_battery_status_init();
    gremlin_output_status_init();

    /* Pre-draw status content so buffer is ready */
    draw_status_content();

    /* One-shot boot timer: 2500ms */
    lv_timer_t *b_timer = lv_timer_create(boot_timer_cb, 2500, NULL);
    if (b_timer) {
        lv_timer_set_repeat_count(b_timer, 1);
    }

    /* Periodic idle check timer: every 1000ms */
    idle_timer = lv_timer_create(idle_check_cb, 1000, NULL);

    return screen;
}
