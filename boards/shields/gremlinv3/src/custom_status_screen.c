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

#define STATUS_CANVAS_SIZE 68

/* -------------------------------------------------------------------------
 * Static Canvas Buffers
 * ------------------------------------------------------------------------- */
static uint8_t boot_buf[LV_CANVAS_BUF_SIZE_INDEXED_1BIT(GREMLIN_BOOT_WIDTH, GREMLIN_BOOT_HEIGHT)];
static uint8_t sleep_buf[LV_CANVAS_BUF_SIZE_INDEXED_1BIT(GREMLIN_SLEEP_WIDTH, GREMLIN_SLEEP_HEIGHT)];
static uint8_t mascot_buf[LV_CANVAS_BUF_SIZE_INDEXED_1BIT(GREMLIN_MASCOT_WIDTH, GREMLIN_MASCOT_HEIGHT)];

static lv_color_t status_cbuf[STATUS_CANVAS_SIZE * STATUS_CANVAS_SIZE];
static lv_color_t status_cbuf_tmp[STATUS_CANVAS_SIZE * STATUS_CANVAS_SIZE];

/* -------------------------------------------------------------------------
 * UI Objects
 * ------------------------------------------------------------------------- */
static lv_obj_t *boot_canvas = NULL;
static lv_obj_t *sleep_canvas = NULL;
static lv_obj_t *active_cont = NULL;
static lv_obj_t *mascot_canvas = NULL;
static lv_obj_t *status_canvas = NULL;

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
 * 90-Degree Canvas Rotation Helper
 * ------------------------------------------------------------------------- */
static void rotate_canvas(lv_obj_t *canvas) {
    memcpy(status_cbuf_tmp, status_cbuf, sizeof(status_cbuf_tmp));
    lv_img_dsc_t img;
    img.data = (void *)status_cbuf_tmp;
    img.header.cf = LV_IMG_CF_TRUE_COLOR;
    img.header.w = STATUS_CANVAS_SIZE;
    img.header.h = STATUS_CANVAS_SIZE;

    lv_canvas_fill_bg(canvas, lv_color_white(), LV_OPA_COVER);
    lv_canvas_transform(canvas, &img, 900, LV_IMG_ZOOM_NONE, -1, 0,
                        STATUS_CANVAS_SIZE / 2, STATUS_CANVAS_SIZE / 2, true);
}

/* -------------------------------------------------------------------------
 * Draw Status Content (Layer, Battery, Output, Footer)
 * ------------------------------------------------------------------------- */
static void draw_status_content(void) {
    if (!status_canvas || !boot_complete) {
        return;
    }

    lv_canvas_fill_bg(status_canvas, lv_color_white(), LV_OPA_COVER);

    // 1. Layer Name (e.g. "GAME", "MOUSE", "SYSTEM")
    lv_draw_label_dsc_t layer_dsc;
    lv_draw_label_dsc_init(&layer_dsc);
    layer_dsc.color = lv_color_black();
    layer_dsc.font = &lv_font_montserrat_14;
    layer_dsc.align = LV_TEXT_ALIGN_CENTER;

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
    lv_canvas_draw_text(status_canvas, 0, 4, STATUS_CANVAS_SIZE, &layer_dsc, layer_text);

    // Divider line below layer
    lv_draw_line_dsc_t line_dsc;
    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = lv_color_black();
    line_dsc.width = 1;
    lv_point_t p1[2] = {{6, 20}, {62, 20}};
    lv_canvas_draw_line(status_canvas, p1, 2, &line_dsc);

    // 2. Battery Status (Icon + Percentage)
    lv_draw_rect_dsc_t rect_blk, rect_wht;
    lv_draw_rect_dsc_init(&rect_blk);
    rect_blk.bg_color = lv_color_black();
    lv_draw_rect_dsc_init(&rect_wht);
    rect_wht.bg_color = lv_color_white();

    // Battery body outline
    lv_canvas_draw_rect(status_canvas, 6, 25, 22, 10, &rect_blk);
    lv_canvas_draw_rect(status_canvas, 7, 26, 20, 8, &rect_wht);
    // Battery terminal
    lv_canvas_draw_rect(status_canvas, 28, 28, 2, 4, &rect_blk);
    // Fill level
    int fill_w = (current_battery_level * 18) / 100;
    if (fill_w > 0) {
        lv_canvas_draw_rect(status_canvas, 8, 27, fill_w, 6, &rect_blk);
    }

    // Battery text
    lv_draw_label_dsc_t bat_dsc;
    lv_draw_label_dsc_init(&bat_dsc);
    bat_dsc.color = lv_color_black();
    bat_dsc.font = &lv_font_montserrat_12;
    bat_dsc.align = LV_TEXT_ALIGN_LEFT;

    char bat_text[12] = {};
    if (is_usb_charging) {
        snprintf(bat_text, sizeof(bat_text), "%d%%+", current_battery_level);
    } else {
        snprintf(bat_text, sizeof(bat_text), "%d%%", current_battery_level);
    }
    lv_canvas_draw_text(status_canvas, 33, 24, 34, &bat_dsc, bat_text);

    // 3. Output Status (e.g. "USB" or "BT 1")
    lv_draw_label_dsc_t out_dsc;
    lv_draw_label_dsc_init(&out_dsc);
    out_dsc.color = lv_color_black();
    out_dsc.font = &lv_font_montserrat_12;
    out_dsc.align = LV_TEXT_ALIGN_CENTER;

    char out_text[16] = {};
    if (is_usb_output) {
        snprintf(out_text, sizeof(out_text), "USB");
    } else {
        snprintf(out_text, sizeof(out_text), "BLE %d", active_ble_profile + 1);
    }
    lv_canvas_draw_text(status_canvas, 0, 38, STATUS_CANVAS_SIZE, &out_dsc, out_text);

    // Divider line above footer
    lv_point_t p2[2] = {{6, 52}, {62, 52}};
    lv_canvas_draw_line(status_canvas, p2, 2, &line_dsc);

    // 4. Footer Branding: "GREMLIN v3"
    lv_draw_label_dsc_t foot_dsc;
    lv_draw_label_dsc_init(&foot_dsc);
    foot_dsc.color = lv_color_black();
    foot_dsc.font = &lv_font_montserrat_10;
    foot_dsc.align = LV_TEXT_ALIGN_CENTER;
    lv_canvas_draw_text(status_canvas, 0, 54, STATUS_CANVAS_SIZE, &foot_dsc, "GREMLIN v3");

    // Rotate 90 degrees clockwise to match vertical panel orientation
    rotate_canvas(status_canvas);
    lv_obj_invalidate(status_canvas);
}

/* -------------------------------------------------------------------------
 * ZMK State Listeners (Layer, Battery, Output)
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

ZMK_DISPLAY_WIDGET_LISTENER(widget_layer_status, struct layer_status_state,
                            layer_status_update_cb, layer_status_get_state)
ZMK_SUBSCRIPTION(widget_layer_status, zmk_layer_state_changed);

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

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, struct battery_status_state,
                            battery_status_update_cb, battery_status_get_state)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_usb_conn_state_changed);
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

ZMK_DISPLAY_WIDGET_LISTENER(widget_output_status, struct output_status_state,
                            output_status_update_cb, output_status_get_state)
ZMK_SUBSCRIPTION(widget_output_status, zmk_endpoint_changed);
#if defined(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(widget_output_status, zmk_ble_active_profile_changed);
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
    if (!screen) {
        return NULL;
    }
    lv_obj_set_size(screen, 160, 68);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);

    /* 1. Full-Body Boot Logo Canvas (Pre-Rotated 160x68 for Vertical Panel) */
    boot_canvas = lv_canvas_create(screen);
    if (boot_canvas) {
        lv_canvas_set_buffer(boot_canvas, boot_buf, GREMLIN_BOOT_WIDTH, GREMLIN_BOOT_HEIGHT, LV_IMG_CF_INDEXED_1BIT);
        lv_canvas_set_palette(boot_canvas, 0, lv_color_white());
        lv_canvas_set_palette(boot_canvas, 1, lv_color_black());
        memcpy(boot_buf + 8, gremlin_boot_bitmap, GREMLIN_BOOT_BITMAP_SIZE);
        lv_obj_align(boot_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    }

    /* 2. Full-Body Standby / Sleep Canvas (Pre-Rotated 160x68 for Vertical Panel) */
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

        /* 3a. Mascot Border Frame (66x66) at Screen Top / User Top */
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

        /* 3c. Divider Line below Mascot (in vertical orientation) */
        lv_obj_t *div1 = lv_obj_create(active_cont);
        if (div1) {
            lv_obj_set_size(div1, 1, 64);
            lv_obj_set_style_bg_color(div1, lv_color_black(), LV_PART_MAIN);
            lv_obj_set_style_border_width(div1, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(div1, 0, LV_PART_MAIN);
            lv_obj_align(div1, LV_ALIGN_TOP_LEFT, 91, 2);
        }

        /* 3d. Status Canvas (68x68 True-Color with 90 deg rotation) */
        status_canvas = lv_canvas_create(active_cont);
        if (status_canvas) {
            lv_canvas_set_buffer(status_canvas, status_cbuf, STATUS_CANVAS_SIZE, STATUS_CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);
            lv_obj_align(status_canvas, LV_ALIGN_TOP_LEFT, 22, 0);
        }
    }

    /* Initialize ZMK widget state subscriptions */
    widget_layer_status_init();
    widget_battery_status_init();
    widget_output_status_init();

    /* One-shot boot timer: 2500ms */
    lv_timer_t *b_timer = lv_timer_create(boot_timer_cb, 2500, NULL);
    if (b_timer) {
        lv_timer_set_repeat_count(b_timer, 1);
    }

    /* Periodic idle check timer: every 1000ms */
    idle_timer = lv_timer_create(idle_check_cb, 1000, NULL);

    return screen;
}
