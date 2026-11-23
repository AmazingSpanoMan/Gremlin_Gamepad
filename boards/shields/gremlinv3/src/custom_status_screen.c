/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#if IS_ENABLED(CONFIG_INPUT)
#include <zephyr/input/input.h>
#endif
#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <lvgl.h>

#include <zmk/display.h>
#include <zmk/display/status_screen.h>
#include <zmk/activity.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
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
#include "gremlin_display_runtime.h"

/* -------------------------------------------------------------------------
 * Layout & Sizing Constants
 * ------------------------------------------------------------------------- */
#define STATUS_PHYS_W 88
#define STATUS_PHYS_H 68
#define USER_CANVAS_W 68
#define USER_CANVAS_H 88

#define GREMLIN_DISP_CFG_MAGIC 0x47445331U

/* -------------------------------------------------------------------------
 * Static Canvas Buffers (All 1-bit monochrome indexed)
 * ------------------------------------------------------------------------- */
static uint8_t boot_buf[LV_CANVAS_BUF_SIZE_INDEXED_1BIT(GREMLIN_BOOT_WIDTH, GREMLIN_BOOT_HEIGHT)];
static uint8_t sleep_buf[LV_CANVAS_BUF_SIZE_INDEXED_1BIT(GREMLIN_SLEEP_WIDTH, GREMLIN_SLEEP_HEIGHT)];
static uint8_t mascot_buf[LV_CANVAS_BUF_SIZE_INDEXED_1BIT(GREMLIN_MASCOT_WIDTH, GREMLIN_MASCOT_HEIGHT)];

static uint8_t status_phys_buf[LV_CANVAS_BUF_SIZE_INDEXED_1BIT(STATUS_PHYS_W, STATUS_PHYS_H)];
static uint8_t user_virt_buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR(USER_CANVAS_W, USER_CANVAS_H)];

/* -------------------------------------------------------------------------
 * UI Objects
 * ------------------------------------------------------------------------- */
static lv_obj_t *boot_canvas = NULL;
static lv_obj_t *sleep_canvas = NULL;
static lv_obj_t *active_cont = NULL;
static lv_obj_t *mascot_box = NULL;
static lv_obj_t *mascot_canvas = NULL;
static lv_obj_t *div_line = NULL;
static lv_obj_t *status_canvas = NULL;
static lv_obj_t *user_canvas = NULL;

static lv_timer_t *boot_timer = NULL;
static lv_timer_t *mascot_timer = NULL;
static lv_timer_t *idle_timer = NULL;

/* -------------------------------------------------------------------------
 * Runtime Display Configuration & Custom Art State
 * ------------------------------------------------------------------------- */
static K_MUTEX_DEFINE(disp_mutex);

static struct gremlin_display_config disp_cfg = {
    .display_sleep_ms = 30000,
    .show_layer_badge = true,
    .show_battery_icon = true,
    .show_battery_percent = true,
    .show_output_badge = true,
    .show_footer = true,
    .show_mascot = true,
    .show_mascot_border = true,
    .boot_duration_ms = 2500,
    .mascot_frame_ms = 350,
    .mascot_frame_count = GREMLIN_MASCOT_FRAME_COUNT,
    .anim_sequence_len = 8,
    .anim_sequence = {0, 0, 1, 0, 0, 2, 3, 2},
};

static uint8_t custom_boot[GREMLIN_BOOT_BITMAP_SIZE];
static bool boot_is_custom = false;
static bool boot_dirty = false;
static bool boot_deleted = false;

static uint8_t custom_sleep[GREMLIN_SLEEP_BITMAP_SIZE];
static bool sleep_is_custom = false;
static bool sleep_dirty = false;
static bool sleep_deleted = false;

static uint8_t custom_mascot[GREMLIN_DISPLAY_MAX_MASCOT_FRAMES][GREMLIN_MASCOT_BITMAP_SIZE];
static bool mascot_is_custom[GREMLIN_DISPLAY_MAX_MASCOT_FRAMES];
static bool mascot_dirty[GREMLIN_DISPLAY_MAX_MASCOT_FRAMES];
static bool mascot_deleted[GREMLIN_DISPLAY_MAX_MASCOT_FRAMES];

static bool cfg_dirty = false;

/* Shared scratch buffer for flash NVS save/load and chunk reads (off RPC/init thread stacks) */
static K_MUTEX_DEFINE(disp_io_mutex);
static uint8_t disp_io_scratch[GREMLIN_BOOT_BITMAP_SIZE];

/* Staging buffer for chunked bitmap uploads so live screens never tear mid-transfer */
static uint8_t upload_staging[GREMLIN_BOOT_BITMAP_SIZE];
static bool upload_active = false;
static enum gremlin_bitmap_target upload_target = GREMLIN_BITMAP_MASCOT;
static uint8_t upload_slot = 0;
static size_t upload_total_size = 0;

/* -------------------------------------------------------------------------
 * Animation & Activity State
 * ------------------------------------------------------------------------- */
static size_t anim_step = 0;
static struct k_spinlock activity_lock;
static int64_t last_activity_time = 0;
static atomic_t display_sleeping = ATOMIC_INIT(0);
static bool is_sleeping = false;
static bool boot_complete = false;

/* Live status state */
static zmk_keymap_layer_index_t current_layer_index = 0;
static const char *current_layer_label = NULL;
static uint8_t current_battery_level = 100;
static bool is_usb_charging = false;
static bool is_usb_output = true;
static int active_ble_profile = 0;

static void set_obj_visible(lv_obj_t *obj, bool visible) {
    if (!obj) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void copy_active_bitmap_locked(enum gremlin_bitmap_target target, uint8_t slot, uint8_t *dst) {
    if (target == GREMLIN_BITMAP_BOOT) {
        memcpy(dst, boot_is_custom ? custom_boot : gremlin_boot_bitmap, GREMLIN_BOOT_BITMAP_SIZE);
    } else if (target == GREMLIN_BITMAP_SLEEP) {
        memcpy(dst, sleep_is_custom ? custom_sleep : gremlin_sleep_bitmap, GREMLIN_SLEEP_BITMAP_SIZE);
    } else {
        if (slot < GREMLIN_DISPLAY_MAX_MASCOT_FRAMES && mascot_is_custom[slot]) {
            memcpy(dst, custom_mascot[slot], GREMLIN_MASCOT_BITMAP_SIZE);
        } else if (slot < GREMLIN_MASCOT_FRAME_COUNT) {
            memcpy(dst, gremlin_mascot_frames[slot], GREMLIN_MASCOT_BITMAP_SIZE);
        } else {
            memset(dst, 0, GREMLIN_MASCOT_BITMAP_SIZE);
        }
    }
}

static size_t bitmap_expected_size(enum gremlin_bitmap_target target) {
    return (target == GREMLIN_BITMAP_MASCOT) ? GREMLIN_MASCOT_BITMAP_SIZE : GREMLIN_BOOT_BITMAP_SIZE;
}

static void reset_default_cfg_locked(void) {
    disp_cfg = (struct gremlin_display_config){
        .display_sleep_ms = 30000,
        .show_layer_badge = true,
        .show_battery_icon = true,
        .show_battery_percent = true,
        .show_output_badge = true,
        .show_footer = true,
        .show_mascot = true,
        .show_mascot_border = true,
        .boot_duration_ms = 2500,
        .mascot_frame_ms = 350,
        .mascot_frame_count = GREMLIN_MASCOT_FRAME_COUNT,
        .anim_sequence_len = 8,
        .anim_sequence = {0, 0, 1, 0, 0, 2, 3, 2},
    };
}

/* -------------------------------------------------------------------------
 * Draw Status Content (1:1 Pixel-Perfect Rotation into Physical Screen)
 * ------------------------------------------------------------------------- */
static void draw_status_content(void) {
    if (!user_canvas || !status_canvas || !boot_complete) {
        return;
    }

    k_mutex_lock(&disp_mutex, K_FOREVER);
    struct gremlin_display_config cfg = disp_cfg;
    k_mutex_unlock(&disp_mutex);

    /* 1. Clear Virtual User Canvas (68 wide x 88 tall, White Background) */
    lv_canvas_fill_bg(user_canvas, lv_color_white(), LV_OPA_COVER);

    lv_draw_rect_dsc_t rect_badge;
    lv_draw_rect_dsc_init(&rect_badge);
    rect_badge.bg_color = lv_color_black();
    rect_badge.radius = 3;

    lv_draw_label_dsc_t lbl_white;
    lv_draw_label_dsc_init(&lbl_white);
    lbl_white.color = lv_color_white();
    lbl_white.font = &lv_font_montserrat_14;
    lbl_white.align = LV_TEXT_ALIGN_CENTER;

    lv_draw_rect_dsc_t rect_blk, rect_wht;
    lv_draw_rect_dsc_init(&rect_blk);
    rect_blk.bg_color = lv_color_black();
    lv_draw_rect_dsc_init(&rect_wht);
    rect_wht.bg_color = lv_color_white();

    lv_draw_label_dsc_t lbl_black;
    lv_draw_label_dsc_init(&lbl_black);
    lbl_black.color = lv_color_black();
    lbl_black.font = &lv_font_montserrat_14;

    /* 2. Layer Name Badge */
    if (cfg.show_layer_badge) {
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
        lv_canvas_draw_text(user_canvas, 3, 5, 62, &lbl_white, layer_text);
    }

    /* 3. Battery Status */
    if (cfg.show_battery_icon) {
        lv_canvas_draw_rect(user_canvas, 5, 29, 22, 11, &rect_blk);
        lv_canvas_draw_rect(user_canvas, 6, 30, 20, 9, &rect_wht);
        lv_canvas_draw_rect(user_canvas, 27, 32, 2, 5, &rect_blk);
        int fill_w = (current_battery_level * 18) / 100;
        if (fill_w > 18) {
            fill_w = 18;
        }
        if (fill_w > 0) {
            lv_canvas_draw_rect(user_canvas, 7, 31, fill_w, 7, &rect_blk);
        }
    }

    if (cfg.show_battery_percent) {
        char bat_text[12] = {};
        if (is_usb_charging) {
            snprintf(bat_text, sizeof(bat_text), "%d%%+", current_battery_level);
        } else {
            snprintf(bat_text, sizeof(bat_text), "%d%%", current_battery_level);
        }
        if (cfg.show_battery_icon) {
            lbl_black.align = LV_TEXT_ALIGN_LEFT;
            lv_canvas_draw_text(user_canvas, 32, 27, 34, &lbl_black, bat_text);
        } else {
            lbl_black.align = LV_TEXT_ALIGN_CENTER;
            lv_canvas_draw_text(user_canvas, 4, 27, 60, &lbl_black, bat_text);
        }
    }

    /* 4. Output Status Badge */
    if (cfg.show_output_badge) {
        lv_canvas_draw_rect(user_canvas, 3, 46, 62, 19, &rect_badge);
        char out_text[16] = {};
        if (is_usb_output) {
            snprintf(out_text, sizeof(out_text), "USB");
        } else {
            snprintf(out_text, sizeof(out_text), "BLE %d", active_ble_profile + 1);
        }
        lv_canvas_draw_text(user_canvas, 3, 48, 62, &lbl_white, out_text);
    }

    /* 5. Footer Divider & Branding */
    if (cfg.show_footer) {
        lv_draw_line_dsc_t line_dsc;
        lv_draw_line_dsc_init(&line_dsc);
        line_dsc.color = lv_color_black();
        line_dsc.width = 1;
        lv_point_t p_div[2] = {{6, 70}, {62, 70}};
        lv_canvas_draw_line(user_canvas, p_div, 2, &line_dsc);

        lbl_black.align = LV_TEXT_ALIGN_CENTER;
        lbl_black.font = &lv_font_montserrat_12;
        lv_canvas_draw_text(user_canvas, 0, 73, 68, &lbl_black, "GREMLIN v3");
    }

    /* 6. Exact 1:1 Pixel Transposition (Rotate 90 deg clockwise into physical 88x68 canvas) */
    uint8_t *dest = status_phys_buf + 8;
    memset(dest, 0, (STATUS_PHYS_W / 8) * STATUS_PHYS_H);
    for (int uy = 0; uy < USER_CANVAS_H; uy++) {
        int px = 87 - uy;
        for (int ux = 0; ux < USER_CANVAS_W; ux++) {
            int py = ux;
            lv_color_t c = lv_canvas_get_px(user_canvas, ux, uy);
            uint32_t c32 = lv_color_to32(c);
            uint8_t r = (c32 >> 16) & 0xFF;
            uint8_t g = (c32 >> 8) & 0xFF;
            uint8_t b = c32 & 0xFF;
            if ((r + g + b) < 384) {
                int byte_idx = py * (STATUS_PHYS_W / 8) + (px >> 3);
                dest[byte_idx] |= (1 << (7 - (px & 7)));
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
    ARG_UNUSED(eh);
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
    ARG_UNUSED(eh);
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
    ARG_UNUSED(eh);
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
 * Timers and Display Refresh Callbacks
 * ------------------------------------------------------------------------- */
static void update_mascot_frame(void) {
    if (!mascot_canvas) {
        return;
    }

    k_mutex_lock(&disp_mutex, K_FOREVER);
    uint8_t seq_len = disp_cfg.anim_sequence_len;
    uint8_t max_frames = CLAMP(disp_cfg.mascot_frame_count, 1, GREMLIN_DISPLAY_MAX_MASCOT_FRAMES);
    uint8_t frame_idx = 0;
    if (seq_len > 0) {
        anim_step = (anim_step + 1U) % seq_len;
        frame_idx = disp_cfg.anim_sequence[anim_step];
    }
    if (frame_idx >= max_frames) {
        frame_idx = 0;
    }
    copy_active_bitmap_locked(GREMLIN_BITMAP_MASCOT, frame_idx, mascot_buf + 8);
    k_mutex_unlock(&disp_mutex);

    lv_obj_invalidate(mascot_canvas);
}

static void mascot_anim_cb(lv_timer_t *timer) {
    ARG_UNUSED(timer);
    update_mascot_frame();
}

static void apply_ui_state(void) {
    k_mutex_lock(&disp_mutex, K_FOREVER);
    struct gremlin_display_config cfg = disp_cfg;
    copy_active_bitmap_locked(GREMLIN_BITMAP_BOOT, 0, boot_buf + 8);
    copy_active_bitmap_locked(GREMLIN_BITMAP_SLEEP, 0, sleep_buf + 8);
    k_mutex_unlock(&disp_mutex);

    if (boot_canvas) {
        lv_obj_invalidate(boot_canvas);
    }
    if (sleep_canvas) {
        lv_obj_invalidate(sleep_canvas);
    }

    if (is_sleeping && cfg.display_sleep_ms == 0U && boot_complete) {
        is_sleeping = false;
        atomic_set(&display_sleeping, 0);
        set_obj_visible(sleep_canvas, false);
        set_obj_visible(active_cont, true);
    }

    set_obj_visible(mascot_canvas, cfg.show_mascot);
    set_obj_visible(div_line, cfg.show_mascot);
    set_obj_visible(mascot_box, cfg.show_mascot && cfg.show_mascot_border);

    anim_step = 0;
    update_mascot_frame();

    if (mascot_timer) {
        uint32_t period = CLAMP(cfg.mascot_frame_ms, 50U, 10000U);
        lv_timer_set_period(mascot_timer, period);
        if (boot_complete && !is_sleeping && cfg.show_mascot && cfg.anim_sequence_len > 0) {
            lv_timer_resume(mascot_timer);
        } else {
            lv_timer_pause(mascot_timer);
        }
    }

    if (boot_complete) {
        draw_status_content();
    }
}

static void apply_ui_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    apply_ui_state();
}

K_WORK_DEFINE(apply_ui_work, apply_ui_work_handler);

static void schedule_ui_refresh(void) {
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &apply_ui_work);
    }
}

static void boot_timer_cb(lv_timer_t *timer) {
    ARG_UNUSED(timer);
    boot_timer = NULL;

    k_spinlock_key_t key = k_spin_lock(&activity_lock);
    last_activity_time = k_uptime_get();
    k_spin_unlock(&activity_lock, key);
    boot_complete = true;

    set_obj_visible(boot_canvas, false);
    set_obj_visible(active_cont, true);

    k_mutex_lock(&disp_mutex, K_FOREVER);
    struct gremlin_display_config cfg = disp_cfg;
    k_mutex_unlock(&disp_mutex);

    if (!mascot_timer) {
        uint32_t period = CLAMP(cfg.mascot_frame_ms, 50U, 10000U);
        mascot_timer = lv_timer_create(mascot_anim_cb, period, NULL);
    }
    apply_ui_state();
}

static void wake_ui_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!boot_complete || !is_sleeping) {
        return;
    }

    is_sleeping = false;
    atomic_set(&display_sleeping, 0);
    set_obj_visible(sleep_canvas, false);
    set_obj_visible(active_cont, true);

    k_mutex_lock(&disp_mutex, K_FOREVER);
    bool resume_anim = disp_cfg.show_mascot && disp_cfg.anim_sequence_len > 0;
    k_mutex_unlock(&disp_mutex);

    if (mascot_timer && resume_anim) {
        lv_timer_resume(mascot_timer);
    }
    draw_status_content();
}

K_WORK_DEFINE(wake_ui_work, wake_ui_work_handler);

static void record_activity(void) {
    k_spinlock_key_t key = k_spin_lock(&activity_lock);
    last_activity_time = k_uptime_get();
    k_spin_unlock(&activity_lock, key);

    if (atomic_get(&display_sleeping) && zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &wake_ui_work);
    }
}

static void idle_check_cb(lv_timer_t *timer) {
    ARG_UNUSED(timer);

    if (!boot_complete || is_sleeping) {
        return;
    }

    k_mutex_lock(&disp_mutex, K_FOREVER);
    uint32_t sleep_ms = disp_cfg.display_sleep_ms;
    k_mutex_unlock(&disp_mutex);

    if (sleep_ms == 0U) {
        return;
    }

    k_spinlock_key_t key = k_spin_lock(&activity_lock);
    int64_t elapsed = k_uptime_get() - last_activity_time;
    k_spin_unlock(&activity_lock, key);

    if (elapsed > (int64_t)sleep_ms) {
        atomic_set(&display_sleeping, 1);

        /* Re-check after publishing display_sleeping to avoid missing concurrent activity */
        key = k_spin_lock(&activity_lock);
        elapsed = k_uptime_get() - last_activity_time;
        k_spin_unlock(&activity_lock, key);

        if (elapsed <= (int64_t)sleep_ms) {
            atomic_set(&display_sleeping, 0);
            return;
        }

        is_sleeping = true;
        set_obj_visible(active_cont, false);
        set_obj_visible(sleep_canvas, true);
        if (mascot_timer) {
            lv_timer_pause(mascot_timer);
        }
    }
}

/* -------------------------------------------------------------------------
 * Key, Activity, and Analog Input Listeners
 * ------------------------------------------------------------------------- */
static int on_sleep_event(const zmk_event_t *eh) {
    if (as_zmk_position_state_changed(eh) != NULL) {
        record_activity();
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_activity_state_changed *act_ev = as_zmk_activity_state_changed(eh);
    if (act_ev != NULL && act_ev->state == ZMK_ACTIVITY_ACTIVE) {
        record_activity();
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(gremlin_sleep_listener, on_sleep_event);
ZMK_SUBSCRIPTION(gremlin_sleep_listener, zmk_position_state_changed);
ZMK_SUBSCRIPTION(gremlin_sleep_listener, zmk_activity_state_changed);

#if IS_ENABLED(CONFIG_INPUT)
static void on_input_event(struct input_event *evt) {
    switch (evt->type) {
    case INPUT_EV_ABS:
    case INPUT_EV_REL:
    case INPUT_EV_KEY:
        record_activity();
        break;
    default:
        break;
    }
}

INPUT_CALLBACK_DEFINE(NULL, on_input_event);
#endif

/* -------------------------------------------------------------------------
 * Runtime Display Configuration & Custom Art API (called by RPC)
 * ------------------------------------------------------------------------- */
void gremlin_display_get_config(struct gremlin_display_config *out) {
    if (!out) {
        return;
    }
    k_mutex_lock(&disp_mutex, K_FOREVER);
    *out = disp_cfg;
    k_mutex_unlock(&disp_mutex);
}

int gremlin_display_set_config(const struct gremlin_display_config *cfg) {
    if (!cfg) {
        return -EINVAL;
    }
    if (cfg->mascot_frame_count == 0 ||
        cfg->mascot_frame_count > GREMLIN_DISPLAY_MAX_MASCOT_FRAMES ||
        cfg->anim_sequence_len > GREMLIN_DISPLAY_MAX_ANIM_SEQ ||
        cfg->mascot_frame_ms < 50U || cfg->mascot_frame_ms > 10000U ||
        cfg->boot_duration_ms > 15000U) {
        return -EINVAL;
    }

    for (uint8_t i = 0; i < cfg->anim_sequence_len; i++) {
        if (cfg->anim_sequence[i] >= cfg->mascot_frame_count) {
            return -EINVAL;
        }
    }

    k_mutex_lock(&disp_mutex, K_FOREVER);
    disp_cfg = *cfg;
    cfg_dirty = true;
    k_mutex_unlock(&disp_mutex);

    record_activity();
    schedule_ui_refresh();
    return 0;
}

int gremlin_display_read_bitmap_chunk(enum gremlin_bitmap_target target, uint8_t slot,
                                      size_t offset, uint8_t *out, size_t req_len,
                                      size_t *actual_len_out, size_t *total_size_out,
                                      bool *is_custom_out) {
    if (!out || !actual_len_out || !total_size_out || !is_custom_out) {
        return -EINVAL;
    }
    if (target == GREMLIN_BITMAP_MASCOT && slot >= GREMLIN_DISPLAY_MAX_MASCOT_FRAMES) {
        return -EINVAL;
    }
    if (target > GREMLIN_BITMAP_SLEEP) {
        return -EINVAL;
    }

    size_t total = bitmap_expected_size(target);
    if (offset > total) {
        return -EINVAL;
    }

    size_t to_copy = MIN(req_len, total - offset);

    k_mutex_lock(&disp_io_mutex, K_FOREVER);
    k_mutex_lock(&disp_mutex, K_FOREVER);
    if (target == GREMLIN_BITMAP_BOOT) {
        *is_custom_out = boot_is_custom;
    } else if (target == GREMLIN_BITMAP_SLEEP) {
        *is_custom_out = sleep_is_custom;
    } else {
        *is_custom_out = mascot_is_custom[slot];
    }
    copy_active_bitmap_locked(target, slot, disp_io_scratch);
    k_mutex_unlock(&disp_mutex);

    if (to_copy > 0) {
        memcpy(out, disp_io_scratch + offset, to_copy);
    }
    k_mutex_unlock(&disp_io_mutex);
    *actual_len_out = to_copy;
    *total_size_out = total;
    return 0;
}

int gremlin_display_write_bitmap_chunk(enum gremlin_bitmap_target target, uint8_t slot,
                                       size_t offset, size_t total_size, const uint8_t *data,
                                       size_t len) {
    if (!data && len > 0) {
        return -EINVAL;
    }
    if (target == GREMLIN_BITMAP_MASCOT && slot >= GREMLIN_DISPLAY_MAX_MASCOT_FRAMES) {
        return -EINVAL;
    }
    if (target > GREMLIN_BITMAP_SLEEP) {
        return -EINVAL;
    }

    size_t expected = bitmap_expected_size(target);
    if (total_size != expected || offset + len > expected) {
        return -EINVAL;
    }

    k_mutex_lock(&disp_mutex, K_FOREVER);
    if (offset == 0 || !upload_active || upload_target != target || upload_slot != slot) {
        memset(upload_staging, 0, sizeof(upload_staging));
        upload_active = true;
        upload_target = target;
        upload_slot = slot;
        upload_total_size = expected;
    }

    if (len > 0) {
        memcpy(upload_staging + offset, data, len);
    }

    bool finished = (offset + len == upload_total_size);
    if (finished) {
        if (target == GREMLIN_BITMAP_BOOT) {
            memcpy(custom_boot, upload_staging, GREMLIN_BOOT_BITMAP_SIZE);
            boot_is_custom = true;
            boot_dirty = true;
            boot_deleted = false;
        } else if (target == GREMLIN_BITMAP_SLEEP) {
            memcpy(custom_sleep, upload_staging, GREMLIN_SLEEP_BITMAP_SIZE);
            sleep_is_custom = true;
            sleep_dirty = true;
            sleep_deleted = false;
        } else {
            memcpy(custom_mascot[slot], upload_staging, GREMLIN_MASCOT_BITMAP_SIZE);
            mascot_is_custom[slot] = true;
            mascot_dirty[slot] = true;
            mascot_deleted[slot] = false;
        }
        upload_active = false;
    }
    k_mutex_unlock(&disp_mutex);

    if (finished) {
        schedule_ui_refresh();
    }
    return 0;
}

int gremlin_display_clear_bitmap(enum gremlin_bitmap_target target, uint8_t slot) {
    if (target > GREMLIN_BITMAP_SLEEP) {
        return -EINVAL;
    }
    if (target == GREMLIN_BITMAP_MASCOT && slot >= GREMLIN_DISPLAY_MAX_MASCOT_FRAMES) {
        return -EINVAL;
    }

    k_mutex_lock(&disp_mutex, K_FOREVER);
    if (upload_active && upload_target == target && upload_slot == slot) {
        upload_active = false;
    }
    if (target == GREMLIN_BITMAP_BOOT) {
        memset(custom_boot, 0, sizeof(custom_boot));
        boot_is_custom = false;
        boot_dirty = false;
        boot_deleted = true;
    } else if (target == GREMLIN_BITMAP_SLEEP) {
        memset(custom_sleep, 0, sizeof(custom_sleep));
        sleep_is_custom = false;
        sleep_dirty = false;
        sleep_deleted = true;
    } else {
        memset(custom_mascot[slot], 0, sizeof(custom_mascot[slot]));
        mascot_is_custom[slot] = false;
        mascot_dirty[slot] = false;
        mascot_deleted[slot] = true;
    }
    k_mutex_unlock(&disp_mutex);

    schedule_ui_refresh();
    return 0;
}

size_t gremlin_display_custom_nvs_bytes(void) {
    size_t total = 0;
    k_mutex_lock(&disp_mutex, K_FOREVER);
    total += sizeof(struct gremlin_display_config);
    if (boot_is_custom) {
        total += GREMLIN_BOOT_BITMAP_SIZE;
    }
    if (sleep_is_custom) {
        total += GREMLIN_SLEEP_BITMAP_SIZE;
    }
    for (int i = 0; i < GREMLIN_DISPLAY_MAX_MASCOT_FRAMES; i++) {
        if (mascot_is_custom[i]) {
            total += GREMLIN_MASCOT_BITMAP_SIZE;
        }
    }
    k_mutex_unlock(&disp_mutex);
    return total;
}

#if IS_ENABLED(CONFIG_SETTINGS)
struct gremlin_disp_nvs_cfg {
    uint32_t magic;
    uint32_t display_sleep_ms;
    uint32_t boot_duration_ms;
    uint32_t mascot_frame_ms;
    uint8_t flags;
    uint8_t mascot_frame_count;
    uint8_t anim_sequence_len;
    uint8_t reserved;
    uint8_t anim_sequence[GREMLIN_DISPLAY_MAX_ANIM_SEQ];
};

static uint8_t pack_disp_flags(const struct gremlin_display_config *c) {
    uint8_t f = 0;
    if (c->show_layer_badge) f |= BIT(0);
    if (c->show_battery_icon) f |= BIT(1);
    if (c->show_battery_percent) f |= BIT(2);
    if (c->show_output_badge) f |= BIT(3);
    if (c->show_footer) f |= BIT(4);
    if (c->show_mascot) f |= BIT(5);
    if (c->show_mascot_border) f |= BIT(6);
    return f;
}

static void unpack_disp_flags(uint8_t f, struct gremlin_display_config *c) {
    c->show_layer_badge = (f & BIT(0)) != 0U;
    c->show_battery_icon = (f & BIT(1)) != 0U;
    c->show_battery_percent = (f & BIT(2)) != 0U;
    c->show_output_badge = (f & BIT(3)) != 0U;
    c->show_footer = (f & BIT(4)) != 0U;
    c->show_mascot = (f & BIT(5)) != 0U;
    c->show_mascot_border = (f & BIT(6)) != 0U;
}

int gremlin_display_save_settings(void) {
    int err = 0;
    struct gremlin_disp_nvs_cfg rec = {0};
    bool save_cfg = false;

    k_mutex_lock(&disp_mutex, K_FOREVER);
    save_cfg = cfg_dirty;
    if (save_cfg) {
        rec.magic = GREMLIN_DISP_CFG_MAGIC;
        rec.display_sleep_ms = disp_cfg.display_sleep_ms;
        rec.boot_duration_ms = disp_cfg.boot_duration_ms;
        rec.mascot_frame_ms = disp_cfg.mascot_frame_ms;
        rec.flags = pack_disp_flags(&disp_cfg);
        rec.mascot_frame_count = disp_cfg.mascot_frame_count;
        rec.anim_sequence_len = disp_cfg.anim_sequence_len;
        memcpy(rec.anim_sequence, disp_cfg.anim_sequence, GREMLIN_DISPLAY_MAX_ANIM_SEQ);
    }
    k_mutex_unlock(&disp_mutex);

    if (save_cfg) {
        int r = settings_save_one("gremlin_disp/cfg", &rec, sizeof(rec));
        if (r == 0) {
            k_mutex_lock(&disp_mutex, K_FOREVER);
            cfg_dirty = false;
            k_mutex_unlock(&disp_mutex);
        } else if (err == 0) {
            err = r;
        }
    }

    bool do_save = false;
    bool do_del = false;

    k_mutex_lock(&disp_io_mutex, K_FOREVER);

    k_mutex_lock(&disp_mutex, K_FOREVER);
    do_save = boot_dirty && boot_is_custom;
    do_del = boot_deleted;
    if (do_save) {
        memcpy(disp_io_scratch, custom_boot, GREMLIN_BOOT_BITMAP_SIZE);
    }
    k_mutex_unlock(&disp_mutex);

    if (do_del) {
        int r = settings_delete("gremlin_disp/boot");
        if (r == 0 || r == -ENOENT) {
            k_mutex_lock(&disp_mutex, K_FOREVER);
            boot_deleted = false;
            k_mutex_unlock(&disp_mutex);
        } else if (err == 0) {
            err = r;
        }
    } else if (do_save) {
        int r = settings_save_one("gremlin_disp/boot", disp_io_scratch, GREMLIN_BOOT_BITMAP_SIZE);
        if (r == 0) {
            k_mutex_lock(&disp_mutex, K_FOREVER);
            boot_dirty = false;
            k_mutex_unlock(&disp_mutex);
        } else if (err == 0) {
            err = r;
        }
    }

    k_mutex_lock(&disp_mutex, K_FOREVER);
    do_save = sleep_dirty && sleep_is_custom;
    do_del = sleep_deleted;
    if (do_save) {
        memcpy(disp_io_scratch, custom_sleep, GREMLIN_SLEEP_BITMAP_SIZE);
    }
    k_mutex_unlock(&disp_mutex);

    if (do_del) {
        int r = settings_delete("gremlin_disp/sleep");
        if (r == 0 || r == -ENOENT) {
            k_mutex_lock(&disp_mutex, K_FOREVER);
            sleep_deleted = false;
            k_mutex_unlock(&disp_mutex);
        } else if (err == 0) {
            err = r;
        }
    } else if (do_save) {
        int r = settings_save_one("gremlin_disp/sleep", disp_io_scratch, GREMLIN_SLEEP_BITMAP_SIZE);
        if (r == 0) {
            k_mutex_lock(&disp_mutex, K_FOREVER);
            sleep_dirty = false;
            k_mutex_unlock(&disp_mutex);
        } else if (err == 0) {
            err = r;
        }
    }

    for (int i = 0; i < GREMLIN_DISPLAY_MAX_MASCOT_FRAMES; i++) {
        char key[24];
        snprintf(key, sizeof(key), "gremlin_disp/m%d", i);

        k_mutex_lock(&disp_mutex, K_FOREVER);
        do_save = mascot_dirty[i] && mascot_is_custom[i];
        do_del = mascot_deleted[i];
        if (do_save) {
            memcpy(disp_io_scratch, custom_mascot[i], GREMLIN_MASCOT_BITMAP_SIZE);
        }
        k_mutex_unlock(&disp_mutex);

        if (do_del) {
            int r = settings_delete(key);
            if (r == 0 || r == -ENOENT) {
                k_mutex_lock(&disp_mutex, K_FOREVER);
                mascot_deleted[i] = false;
                k_mutex_unlock(&disp_mutex);
            } else if (err == 0) {
                err = r;
            }
        } else if (do_save) {
            int r = settings_save_one(key, disp_io_scratch, GREMLIN_MASCOT_BITMAP_SIZE);
            if (r == 0) {
                k_mutex_lock(&disp_mutex, K_FOREVER);
                mascot_dirty[i] = false;
                k_mutex_unlock(&disp_mutex);
            } else if (err == 0) {
                err = r;
            }
        }
    }

    k_mutex_unlock(&disp_io_mutex);
    return err;
}

int gremlin_display_reset_settings(void) {
    int err = 0;
    const char *keys[] = {"gremlin_disp/cfg", "gremlin_disp/boot", "gremlin_disp/sleep"};
    for (size_t k = 0; k < ARRAY_SIZE(keys); k++) {
        int r = settings_delete(keys[k]);
        if (r < 0 && r != -ENOENT && err == 0) {
            err = r;
        }
    }
    for (int i = 0; i < GREMLIN_DISPLAY_MAX_MASCOT_FRAMES; i++) {
        char key[24];
        snprintf(key, sizeof(key), "gremlin_disp/m%d", i);
        int r = settings_delete(key);
        if (r < 0 && r != -ENOENT && err == 0) {
            err = r;
        }
    }

    k_mutex_lock(&disp_mutex, K_FOREVER);
    reset_default_cfg_locked();
    cfg_dirty = false;
    boot_is_custom = false;
    boot_dirty = false;
    boot_deleted = false;
    sleep_is_custom = false;
    sleep_dirty = false;
    sleep_deleted = false;
    memset(custom_boot, 0, sizeof(custom_boot));
    memset(custom_sleep, 0, sizeof(custom_sleep));
    for (int i = 0; i < GREMLIN_DISPLAY_MAX_MASCOT_FRAMES; i++) {
        mascot_is_custom[i] = false;
        mascot_dirty[i] = false;
        mascot_deleted[i] = false;
        memset(custom_mascot[i], 0, sizeof(custom_mascot[i]));
    }
    upload_active = false;
    k_mutex_unlock(&disp_mutex);

    schedule_ui_refresh();
    return err;
}

static int gremlin_disp_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                     void *cb_arg) {
    if (!name) {
        return -ENOENT;
    }

    if (strcmp(name, "cfg") == 0) {
        struct gremlin_disp_nvs_cfg rec;
        if (len != sizeof(rec)) {
            return -EINVAL;
        }
        int rc = read_cb(cb_arg, &rec, sizeof(rec));
        if (rc < 0 || rec.magic != GREMLIN_DISP_CFG_MAGIC) {
            return rc < 0 ? rc : -EINVAL;
        }
        k_mutex_lock(&disp_mutex, K_FOREVER);
        disp_cfg.display_sleep_ms = rec.display_sleep_ms;
        disp_cfg.boot_duration_ms = MIN(rec.boot_duration_ms, 15000U);
        disp_cfg.mascot_frame_ms = CLAMP(rec.mascot_frame_ms, 50U, 10000U);
        unpack_disp_flags(rec.flags, &disp_cfg);
        disp_cfg.mascot_frame_count =
            CLAMP(rec.mascot_frame_count, 1, GREMLIN_DISPLAY_MAX_MASCOT_FRAMES);
        disp_cfg.anim_sequence_len = MIN(rec.anim_sequence_len, GREMLIN_DISPLAY_MAX_ANIM_SEQ);
        memcpy(disp_cfg.anim_sequence, rec.anim_sequence, GREMLIN_DISPLAY_MAX_ANIM_SEQ);
        cfg_dirty = false;
        k_mutex_unlock(&disp_mutex);
        return 0;
    }

    if (strcmp(name, "boot") == 0 && len == GREMLIN_BOOT_BITMAP_SIZE) {
        k_mutex_lock(&disp_io_mutex, K_FOREVER);
        int rc = read_cb(cb_arg, disp_io_scratch, GREMLIN_BOOT_BITMAP_SIZE);
        if (rc >= 0) {
            k_mutex_lock(&disp_mutex, K_FOREVER);
            memcpy(custom_boot, disp_io_scratch, GREMLIN_BOOT_BITMAP_SIZE);
            boot_is_custom = true;
            boot_dirty = false;
            boot_deleted = false;
            k_mutex_unlock(&disp_mutex);
            rc = 0;
        }
        k_mutex_unlock(&disp_io_mutex);
        return rc;
    }

    if (strcmp(name, "sleep") == 0 && len == GREMLIN_SLEEP_BITMAP_SIZE) {
        k_mutex_lock(&disp_io_mutex, K_FOREVER);
        int rc = read_cb(cb_arg, disp_io_scratch, GREMLIN_SLEEP_BITMAP_SIZE);
        if (rc >= 0) {
            k_mutex_lock(&disp_mutex, K_FOREVER);
            memcpy(custom_sleep, disp_io_scratch, GREMLIN_SLEEP_BITMAP_SIZE);
            sleep_is_custom = true;
            sleep_dirty = false;
            sleep_deleted = false;
            k_mutex_unlock(&disp_mutex);
            rc = 0;
        }
        k_mutex_unlock(&disp_io_mutex);
        return rc;
    }

    if (name[0] == 'm' && name[1] >= '0' && name[1] <= ('0' + GREMLIN_DISPLAY_MAX_MASCOT_FRAMES - 1) &&
        name[2] == '\0' && len == GREMLIN_MASCOT_BITMAP_SIZE) {
        int slot = name[1] - '0';
        k_mutex_lock(&disp_io_mutex, K_FOREVER);
        int rc = read_cb(cb_arg, disp_io_scratch, GREMLIN_MASCOT_BITMAP_SIZE);
        if (rc >= 0) {
            k_mutex_lock(&disp_mutex, K_FOREVER);
            memcpy(custom_mascot[slot], disp_io_scratch, GREMLIN_MASCOT_BITMAP_SIZE);
            mascot_is_custom[slot] = true;
            mascot_dirty[slot] = false;
            mascot_deleted[slot] = false;
            k_mutex_unlock(&disp_mutex);
            rc = 0;
        }
        k_mutex_unlock(&disp_io_mutex);
        return rc;
    }

    return -ENOENT;
}

static int gremlin_disp_settings_commit(void) {
    schedule_ui_refresh();
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(gremlin_disp, "gremlin_disp", NULL, gremlin_disp_settings_set,
                               gremlin_disp_settings_commit, NULL);
#else
int gremlin_display_save_settings(void) { return 0; }
int gremlin_display_reset_settings(void) {
    k_mutex_lock(&disp_mutex, K_FOREVER);
    reset_default_cfg_locked();
    cfg_dirty = false;
    boot_is_custom = false;
    sleep_is_custom = false;
    for (int i = 0; i < GREMLIN_DISPLAY_MAX_MASCOT_FRAMES; i++) {
        mascot_is_custom[i] = false;
    }
    k_mutex_unlock(&disp_mutex);
    schedule_ui_refresh();
    return 0;
}
#endif

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

    k_mutex_lock(&disp_mutex, K_FOREVER);
    struct gremlin_display_config cfg = disp_cfg;
    copy_active_bitmap_locked(GREMLIN_BITMAP_BOOT, 0, boot_buf + 8);
    copy_active_bitmap_locked(GREMLIN_BITMAP_SLEEP, 0, sleep_buf + 8);
    copy_active_bitmap_locked(GREMLIN_BITMAP_MASCOT, 0, mascot_buf + 8);
    k_mutex_unlock(&disp_mutex);

    /* 1. Full-Body Boot Logo Canvas (160x68 Native Landscape) */
    boot_canvas = lv_canvas_create(screen);
    if (boot_canvas) {
        lv_canvas_set_buffer(boot_canvas, boot_buf, GREMLIN_BOOT_WIDTH, GREMLIN_BOOT_HEIGHT,
                             LV_IMG_CF_INDEXED_1BIT);
        lv_canvas_set_palette(boot_canvas, 0, lv_color_white());
        lv_canvas_set_palette(boot_canvas, 1, lv_color_black());
        lv_obj_align(boot_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    }

    /* 2. Full-Body Standby / Sleep Canvas (160x68 Native Landscape) */
    sleep_canvas = lv_canvas_create(screen);
    if (sleep_canvas) {
        lv_canvas_set_buffer(sleep_canvas, sleep_buf, GREMLIN_SLEEP_WIDTH, GREMLIN_SLEEP_HEIGHT,
                             LV_IMG_CF_INDEXED_1BIT);
        lv_canvas_set_palette(sleep_canvas, 0, lv_color_white());
        lv_canvas_set_palette(sleep_canvas, 1, lv_color_black());
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
        lv_obj_add_flag(active_cont, LV_OBJ_FLAG_HIDDEN);

        /* 3a. Mascot Border Frame (66x66) at User Top */
        mascot_box = lv_obj_create(active_cont);
        if (mascot_box) {
            lv_obj_set_size(mascot_box, 66, 66);
            lv_obj_set_style_pad_all(mascot_box, 0, LV_PART_MAIN);
            lv_obj_set_style_border_width(mascot_box, 1, LV_PART_MAIN);
            lv_obj_set_style_border_color(mascot_box, lv_color_black(), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(mascot_box, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_clear_flag(mascot_box, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_align(mascot_box, LV_ALIGN_TOP_LEFT, 93, 1);
            set_obj_visible(mascot_box, cfg.show_mascot && cfg.show_mascot_border);
        }

        /* 3b. Mascot Canvas (64x64) inside Border Frame */
        mascot_canvas = lv_canvas_create(active_cont);
        if (mascot_canvas) {
            lv_canvas_set_buffer(mascot_canvas, mascot_buf, GREMLIN_MASCOT_WIDTH,
                                 GREMLIN_MASCOT_HEIGHT, LV_IMG_CF_INDEXED_1BIT);
            lv_canvas_set_palette(mascot_canvas, 0, lv_color_white());
            lv_canvas_set_palette(mascot_canvas, 1, lv_color_black());
            lv_obj_align(mascot_canvas, LV_ALIGN_TOP_LEFT, 94, 2);
            set_obj_visible(mascot_canvas, cfg.show_mascot);
        }

        /* 3c. Divider Line between Mascot and Status */
        div_line = lv_obj_create(active_cont);
        if (div_line) {
            lv_obj_set_size(div_line, 1, 64);
            lv_obj_set_style_bg_color(div_line, lv_color_black(), LV_PART_MAIN);
            lv_obj_set_style_border_width(div_line, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(div_line, 0, LV_PART_MAIN);
            lv_obj_align(div_line, LV_ALIGN_TOP_LEFT, 91, 2);
            set_obj_visible(div_line, cfg.show_mascot);
        }

        /* 3d. Status Canvas (88x68 1-bit Physical) */
        status_canvas = lv_canvas_create(active_cont);
        if (status_canvas) {
            lv_canvas_set_buffer(status_canvas, status_phys_buf, STATUS_PHYS_W, STATUS_PHYS_H,
                                 LV_IMG_CF_INDEXED_1BIT);
            lv_canvas_set_palette(status_canvas, 0, lv_color_white());
            lv_canvas_set_palette(status_canvas, 1, lv_color_black());
            lv_obj_align(status_canvas, LV_ALIGN_TOP_LEFT, 2, 0);
        }

        /* 3e. Hidden Virtual User Canvas (68x88 for Razor-Sharp Layout) */
        user_canvas = lv_canvas_create(active_cont);
        if (user_canvas) {
            lv_canvas_set_buffer(user_canvas, user_virt_buf, USER_CANVAS_W, USER_CANVAS_H,
                                 LV_IMG_CF_TRUE_COLOR);
            lv_obj_add_flag(user_canvas, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Initialize ZMK widget state subscriptions */
    gremlin_layer_status_init();
    gremlin_battery_status_init();
    gremlin_output_status_init();

    uint32_t boot_ms = cfg.boot_duration_ms > 0 ? cfg.boot_duration_ms : 1U;
    boot_timer = lv_timer_create(boot_timer_cb, boot_ms, NULL);
    if (boot_timer) {
        lv_timer_set_repeat_count(boot_timer, 1);
    }

    /* Periodic idle check timer: every 1000ms */
    idle_timer = lv_timer_create(idle_check_cb, 1000, NULL);

    return screen;
}
