/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

#include <drivers/behavior.h>
#include <zmk/activity.h>
#include <zmk/behavior.h>
#include <zmk/combo.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/gremlin_behaviors.h>
#include <zmk/keymap.h>
#include <zmk/studio/rpc.h>

#include <analog-stick/analog_stick_runtime.h>

#include "gremlin_display_runtime.h"

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#define GREMLIN_PROTOCOL_VERSION 1U
#define GREMLIN_BITMAP_CHUNK_SIZE 256U
#define GREMLIN_L3_POSITION 22U

ZMK_RPC_SUBSYSTEM(gremlin)

#define GREMLIN_RESPONSE(type, ...) ZMK_RPC_RESPONSE(gremlin, type, __VA_ARGS__)

static atomic_t l3_pressed = ATOMIC_INIT(0);

static int gremlin_l3_tracker_cb(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev && ev->position == GREMLIN_L3_POSITION) {
        atomic_set(&l3_pressed, ev->state ? 1 : 0);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(gremlin_l3_tracker, gremlin_l3_tracker_cb);
ZMK_SUBSCRIPTION(gremlin_l3_tracker, zmk_position_state_changed);

static zmk_gremlin_GremlinStatusCode errno_to_gremlin_status(int err) {
    switch (err) {
    case 0:
        return zmk_gremlin_GremlinStatusCode_GREMLIN_STATUS_OK;
    case -EINVAL:
        return zmk_gremlin_GremlinStatusCode_GREMLIN_STATUS_ERR_INVALID_ARG;
    case -ENODEV:
    case -ENOENT:
        return zmk_gremlin_GremlinStatusCode_GREMLIN_STATUS_ERR_INVALID_SLOT;
    case -ENOTSUP:
        return zmk_gremlin_GremlinStatusCode_GREMLIN_STATUS_ERR_NOT_SUPPORTED;
    case -ENOSPC:
    case -ENOMEM:
        return zmk_gremlin_GremlinStatusCode_GREMLIN_STATUS_ERR_NO_SPACE;
    default:
        return zmk_gremlin_GremlinStatusCode_GREMLIN_STATUS_ERR_GENERIC;
    }
}

static void binding_to_proto(const struct zmk_behavior_binding *src, zmk_gremlin_BindingSpec *dst) {
    *dst = (zmk_gremlin_BindingSpec)zmk_gremlin_BindingSpec_init_zero;
    if (src && src->behavior_dev) {
        dst->behavior_id = (int32_t)zmk_behavior_get_local_id(src->behavior_dev);
        dst->param1 = src->param1;
        dst->param2 = src->param2;
    } else {
        dst->behavior_id = -1;
    }
}

static int proto_to_binding(const zmk_gremlin_BindingSpec *src, struct zmk_behavior_binding *dst) {
    *dst = (struct zmk_behavior_binding){0};
    if (!src || src->behavior_id < 0 || src->behavior_id > UINT16_MAX) {
        return -EINVAL;
    }

    const char *dev_name = zmk_behavior_find_behavior_name_from_local_id((uint16_t)src->behavior_id);
    if (!dev_name) {
        return -ENOENT;
    }

    dst->behavior_dev = dev_name;
    dst->param1 = src->param1;
    dst->param2 = src->param2;
    return 0;
}

static uint32_t get_storage_total_bytes(void) {
#if DT_NODE_EXISTS(DT_NODELABEL(storage_partition))
    return (uint32_t)DT_REG_SIZE(DT_NODELABEL(storage_partition));
#else
    return 32768U;
#endif
}

static uint32_t estimate_gremlin_storage_used(void) {
    size_t bytes = 128U; /* timeout + analog stick baseline */
    bytes += zmk_combo_runtime_slot_count() * 40U;
    bytes += zmk_gremlin_hold_tap_count() * 40U;
    bytes += zmk_gremlin_tap_dance_count() * 48U;
    bytes += zmk_gremlin_macro_count() * 160U;
    bytes += gremlin_display_custom_nvs_bytes();
    return (uint32_t)bytes;
}

static zmk_studio_Response get_capabilities(const zmk_studio_Request *req) {
    ARG_UNUSED(req);

    zmk_gremlin_Capabilities cap = zmk_gremlin_Capabilities_init_zero;
    cap.protocol_version = GREMLIN_PROTOCOL_VERSION;
    cap.max_combos = (uint32_t)zmk_combo_runtime_slot_count();
    cap.max_hold_taps = (uint32_t)zmk_gremlin_hold_tap_count();
    cap.max_tap_dances = (uint32_t)zmk_gremlin_tap_dance_count();
    cap.max_macros = (uint32_t)zmk_gremlin_macro_count();
    cap.max_mascot_frames = GREMLIN_DISPLAY_MAX_MASCOT_FRAMES;
    cap.max_stick_layers = (uint32_t)zmk_analog_stick_layer_count(0);
    cap.max_macro_steps = ZMK_GREMLIN_MAX_MACRO_STEPS;
    cap.max_tap_dance_steps = ZMK_GREMLIN_MAX_TAP_DANCE_STEPS;
    cap.max_combo_keys = ZMK_COMBO_RUNTIME_MAX_KEYS;
    cap.bitmap_chunk_size = GREMLIN_BITMAP_CHUNK_SIZE;
    cap.storage_total_bytes = get_storage_total_bytes();
    cap.storage_used_bytes = estimate_gremlin_storage_used();

    return GREMLIN_RESPONSE(get_capabilities, cap);
}

static zmk_studio_Response get_sleep_config(const zmk_studio_Request *req) {
    ARG_UNUSED(req);

    struct gremlin_display_config dcfg;
    gremlin_display_get_config(&dcfg);

    zmk_gremlin_SleepConfig resp = zmk_gremlin_SleepConfig_init_zero;
    resp.display_sleep_ms = dcfg.display_sleep_ms;
    resp.idle_timeout_ms = zmk_activity_get_idle_timeout_ms();
    resp.deep_sleep_ms = zmk_activity_get_sleep_timeout_ms();

    return GREMLIN_RESPONSE(get_sleep_config, resp);
}

static zmk_studio_Response set_sleep_config(const zmk_studio_Request *req) {
    const zmk_gremlin_SleepConfig *cfg = &req->subsystem.gremlin.request_type.set_sleep_config;

    struct gremlin_display_config dcfg;
    gremlin_display_get_config(&dcfg);
    dcfg.display_sleep_ms = cfg->display_sleep_ms;

    int err = gremlin_display_set_config(&dcfg);
    if (err == 0) {
        err = zmk_activity_set_timeouts(cfg->idle_timeout_ms, cfg->deep_sleep_ms);
    }

    return GREMLIN_RESPONSE(set_sleep_config, errno_to_gremlin_status(err));
}

static zmk_studio_Response get_display_config(const zmk_studio_Request *req) {
    ARG_UNUSED(req);

    struct gremlin_display_config dcfg;
    gremlin_display_get_config(&dcfg);

    zmk_gremlin_DisplayConfig resp = zmk_gremlin_DisplayConfig_init_zero;
    resp.show_layer_badge = dcfg.show_layer_badge;
    resp.show_battery_icon = dcfg.show_battery_icon;
    resp.show_battery_percent = dcfg.show_battery_percent;
    resp.show_output_badge = dcfg.show_output_badge;
    resp.show_footer = dcfg.show_footer;
    resp.show_mascot = dcfg.show_mascot;
    resp.show_mascot_border = dcfg.show_mascot_border;
    resp.boot_duration_ms = dcfg.boot_duration_ms;
    resp.mascot_frame_ms = dcfg.mascot_frame_ms;
    resp.mascot_frame_count = dcfg.mascot_frame_count;
    resp.anim_sequence_count =
        (pb_size_t)MIN(dcfg.anim_sequence_len, (uint8_t)ARRAY_SIZE(resp.anim_sequence));
    for (pb_size_t i = 0; i < resp.anim_sequence_count; i++) {
        resp.anim_sequence[i] = dcfg.anim_sequence[i];
    }

    return GREMLIN_RESPONSE(get_display_config, resp);
}

static zmk_studio_Response set_display_config(const zmk_studio_Request *req) {
    const zmk_gremlin_DisplayConfig *in = &req->subsystem.gremlin.request_type.set_display_config;

    struct gremlin_display_config dcfg;
    gremlin_display_get_config(&dcfg);

    dcfg.show_layer_badge = in->show_layer_badge;
    dcfg.show_battery_icon = in->show_battery_icon;
    dcfg.show_battery_percent = in->show_battery_percent;
    dcfg.show_output_badge = in->show_output_badge;
    dcfg.show_footer = in->show_footer;
    dcfg.show_mascot = in->show_mascot;
    dcfg.show_mascot_border = in->show_mascot_border;
    dcfg.boot_duration_ms = in->boot_duration_ms;
    dcfg.mascot_frame_ms = in->mascot_frame_ms;
    dcfg.mascot_frame_count = (uint8_t)in->mascot_frame_count;
    dcfg.anim_sequence_len = (uint8_t)MIN(in->anim_sequence_count,
                                          (pb_size_t)GREMLIN_DISPLAY_MAX_ANIM_SEQ);
    memset(dcfg.anim_sequence, 0, sizeof(dcfg.anim_sequence));
    for (uint8_t i = 0; i < dcfg.anim_sequence_len; i++) {
        dcfg.anim_sequence[i] = (uint8_t)in->anim_sequence[i];
    }

    int err = gremlin_display_set_config(&dcfg);
    return GREMLIN_RESPONSE(set_display_config, errno_to_gremlin_status(err));
}

static zmk_studio_Response get_stick_config(const zmk_studio_Request *req) {
    ARG_UNUSED(req);

    struct zmk_analog_stick_calibration cal;
    int err = zmk_analog_stick_get_calibration(0, &cal);
    if (err < 0) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    zmk_gremlin_StickConfig resp = zmk_gremlin_StickConfig_init_zero;
    resp.x_min = cal.x_min;
    resp.x_center = cal.x_center;
    resp.x_max = cal.x_max;
    resp.y_min = cal.y_min;
    resp.y_center = cal.y_center;
    resp.y_max = cal.y_max;
    resp.invert_x = cal.invert_x;
    resp.invert_y = cal.invert_y;
    resp.deadzone_percent = cal.deadzone_percent;

    return GREMLIN_RESPONSE(get_stick_config, resp);
}

static zmk_studio_Response set_stick_config(const zmk_studio_Request *req) {
    const zmk_gremlin_StickConfig *in = &req->subsystem.gremlin.request_type.set_stick_config;

    struct zmk_analog_stick_calibration cal;
    int err = zmk_analog_stick_get_calibration(0, &cal);
    if (err == 0) {
        cal.x_min = in->x_min;
        cal.x_center = in->x_center;
        cal.x_max = in->x_max;
        cal.y_min = in->y_min;
        cal.y_center = in->y_center;
        cal.y_max = in->y_max;
        cal.invert_x = in->invert_x;
        cal.invert_y = in->invert_y;
        cal.deadzone_percent = in->deadzone_percent;
        err = zmk_analog_stick_set_calibration(0, &cal);
    }

    return GREMLIN_RESPONSE(set_stick_config, errno_to_gremlin_status(err));
}

static zmk_studio_Response get_stick_layer_config(const zmk_studio_Request *req) {
    uint32_t layer_idx = req->subsystem.gremlin.request_type.get_stick_layer_config;

    struct zmk_analog_stick_layer_config cfg;
    int err = zmk_analog_stick_get_layer_config(0, layer_idx, &cfg);
    if (err < 0) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    zmk_gremlin_StickLayerConfig resp = zmk_gremlin_StickLayerConfig_init_zero;
    resp.layer_index = cfg.layer_index;
    resp.mode = (zmk_gremlin_StickMode)cfg.mode;
    resp.up_position = cfg.up_position;
    resp.down_position = cfg.down_position;
    resp.left_position = cfg.left_position;
    resp.right_position = cfg.right_position;
    resp.switch_activation_point = (uint32_t)MAX(cfg.switch_activation_point, 0);
    resp.switch_hysteresis = (uint32_t)MAX(cfg.switch_hysteresis, 0);
    resp.mouse_min_speed = (uint32_t)MAX(cfg.mouse_min_speed, 0);
    resp.mouse_max_speed = (uint32_t)MAX(cfg.mouse_max_speed, 0);
    resp.scroll_divisor = (uint32_t)MAX(cfg.scroll_divisor, 0);

    return GREMLIN_RESPONSE(get_stick_layer_config, resp);
}

static zmk_studio_Response set_stick_layer_config(const zmk_studio_Request *req) {
    const zmk_gremlin_StickLayerConfig *in =
        &req->subsystem.gremlin.request_type.set_stick_layer_config;

    struct zmk_analog_stick_layer_config cfg;
    int err = zmk_analog_stick_get_layer_config(0, in->layer_index, &cfg);
    if (err == 0) {
        cfg.layer_index = in->layer_index;
        cfg.enabled = true;
        cfg.mode = (uint8_t)in->mode;
        cfg.up_position = in->up_position;
        cfg.down_position = in->down_position;
        cfg.left_position = in->left_position;
        cfg.right_position = in->right_position;
        cfg.switch_activation_point = (int32_t)in->switch_activation_point;
        cfg.switch_hysteresis = (int32_t)in->switch_hysteresis;
        cfg.mouse_min_speed = (int32_t)in->mouse_min_speed;
        cfg.mouse_max_speed = (int32_t)in->mouse_max_speed;
        cfg.scroll_divisor = (int32_t)in->scroll_divisor;
        err = zmk_analog_stick_set_layer_config(0, &cfg);
    }

    return GREMLIN_RESPONSE(set_stick_layer_config, errno_to_gremlin_status(err));
}

static zmk_studio_Response get_stick_telemetry(const zmk_studio_Request *req) {
    ARG_UNUSED(req);

    struct zmk_analog_stick_telemetry tel;
    int err = zmk_analog_stick_get_telemetry(0, &tel);
    if (err < 0) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    zmk_gremlin_StickTelemetry resp = zmk_gremlin_StickTelemetry_init_zero;
    resp.raw_x = tel.raw_x;
    resp.raw_y = tel.raw_y;
    resp.out_x = tel.out_x;
    resp.out_y = tel.out_y;
    resp.active_layer = zmk_keymap_highest_layer_active();
    resp.l3_pressed = (atomic_get(&l3_pressed) != 0);

    return GREMLIN_RESPONSE(get_stick_telemetry, resp);
}

static zmk_studio_Response get_combo(const zmk_studio_Request *req) {
    uint32_t slot = req->subsystem.gremlin.request_type.get_combo;

    struct zmk_combo_runtime_slot cfg;
    int err = zmk_combo_get_slot(slot, &cfg);
    if (err < 0) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    zmk_gremlin_ComboConfig resp = zmk_gremlin_ComboConfig_init_zero;
    resp.index = slot;
    resp.enabled = cfg.enabled;
    resp.timeout_ms = (uint32_t)MAX(cfg.timeout_ms, 0);
    resp.require_prior_idle_ms = (uint32_t)MAX(cfg.require_prior_idle_ms, (int16_t)0);
    resp.layer_mask = cfg.layer_mask;
    resp.slow_release = cfg.slow_release;
    resp.key_positions_count =
        (pb_size_t)CLAMP(cfg.key_position_len, (int16_t)0, (int16_t)ARRAY_SIZE(resp.key_positions));
    for (pb_size_t i = 0; i < resp.key_positions_count; i++) {
        resp.key_positions[i] = cfg.key_positions[i];
    }
    resp.has_binding = true;
    binding_to_proto(&cfg.behavior, &resp.binding);

    return GREMLIN_RESPONSE(get_combo, resp);
}

static zmk_studio_Response set_combo(const zmk_studio_Request *req) {
    const zmk_gremlin_ComboConfig *in = &req->subsystem.gremlin.request_type.set_combo;

    struct zmk_combo_runtime_slot cfg = {0};
    cfg.enabled = in->enabled;
    cfg.timeout_ms = (int32_t)in->timeout_ms;
    cfg.require_prior_idle_ms = (int16_t)MIN(in->require_prior_idle_ms, (uint32_t)INT16_MAX);
    cfg.layer_mask = in->layer_mask;
    cfg.slow_release = in->slow_release;
    cfg.key_position_len =
        (int16_t)MIN(in->key_positions_count, (pb_size_t)ZMK_COMBO_RUNTIME_MAX_KEYS);
    for (int i = 0; i < cfg.key_position_len; i++) {
        cfg.key_positions[i] = in->key_positions[i];
    }

    if (in->has_binding && in->binding.behavior_id >= 0) {
        int b_err = proto_to_binding(&in->binding, &cfg.behavior);
        if (b_err < 0) {
            return GREMLIN_RESPONSE(set_combo, errno_to_gremlin_status(b_err));
        }
    } else if (in->enabled) {
        return GREMLIN_RESPONSE(set_combo, zmk_gremlin_GremlinStatusCode_GREMLIN_STATUS_ERR_INVALID_ARG);
    }

    int err = zmk_combo_set_slot(in->index, &cfg);
    return GREMLIN_RESPONSE(set_combo, errno_to_gremlin_status(err));
}

static zmk_studio_Response get_hold_tap(const zmk_studio_Request *req) {
    uint32_t slot = req->subsystem.gremlin.request_type.get_hold_tap;

    struct zmk_gremlin_hold_tap_slot cfg;
    int err = zmk_gremlin_hold_tap_get(slot, &cfg);
    if (err < 0) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    zmk_gremlin_HoldTapConfig resp = zmk_gremlin_HoldTapConfig_init_zero;
    resp.index = slot;
    resp.enabled = cfg.enabled;
    resp.tapping_term_ms = (uint32_t)MAX(cfg.tapping_term_ms, 0);
    resp.quick_tap_ms = cfg.quick_tap_ms > 0 ? (uint32_t)cfg.quick_tap_ms : 0U;
    resp.require_prior_idle_ms =
        cfg.require_prior_idle_ms > 0 ? (uint32_t)cfg.require_prior_idle_ms : 0U;
    resp.flavor = (zmk_gremlin_HoldTapFlavor)cfg.flavor;
    resp.retro_tap = cfg.retro_tap;
    resp.has_hold_binding = true;
    binding_to_proto(&cfg.hold_binding, &resp.hold_binding);
    resp.has_tap_binding = true;
    binding_to_proto(&cfg.tap_binding, &resp.tap_binding);

    return GREMLIN_RESPONSE(get_hold_tap, resp);
}

static zmk_studio_Response set_hold_tap(const zmk_studio_Request *req) {
    const zmk_gremlin_HoldTapConfig *in = &req->subsystem.gremlin.request_type.set_hold_tap;

    struct zmk_gremlin_hold_tap_slot cfg = {0};
    cfg.enabled = in->enabled;
    cfg.tapping_term_ms = (int32_t)in->tapping_term_ms;
    cfg.quick_tap_ms = in->quick_tap_ms > 0U ? (int32_t)MIN(in->quick_tap_ms, 60000U) : -1;
    cfg.require_prior_idle_ms =
        in->require_prior_idle_ms > 0U ? (int32_t)MIN(in->require_prior_idle_ms, 60000U) : -1;
    cfg.flavor = (uint8_t)in->flavor;
    cfg.retro_tap = in->retro_tap;

    if (in->has_hold_binding && in->hold_binding.behavior_id >= 0) {
        int b_err = proto_to_binding(&in->hold_binding, &cfg.hold_binding);
        if (b_err < 0) {
            return GREMLIN_RESPONSE(set_hold_tap, errno_to_gremlin_status(b_err));
        }
    }
    if (in->has_tap_binding && in->tap_binding.behavior_id >= 0) {
        int b_err = proto_to_binding(&in->tap_binding, &cfg.tap_binding);
        if (b_err < 0) {
            return GREMLIN_RESPONSE(set_hold_tap, errno_to_gremlin_status(b_err));
        }
    }

    int err = zmk_gremlin_hold_tap_set(in->index, &cfg);
    return GREMLIN_RESPONSE(set_hold_tap, errno_to_gremlin_status(err));
}

static zmk_studio_Response get_tap_dance(const zmk_studio_Request *req) {
    uint32_t slot = req->subsystem.gremlin.request_type.get_tap_dance;

    struct zmk_gremlin_tap_dance_slot cfg;
    int err = zmk_gremlin_tap_dance_get(slot, &cfg);
    if (err < 0) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    zmk_gremlin_TapDanceConfig resp = zmk_gremlin_TapDanceConfig_init_zero;
    resp.index = slot;
    resp.enabled = cfg.enabled;
    resp.tapping_term_ms = cfg.tapping_term_ms;
    resp.bindings_count =
        (pb_size_t)MIN(cfg.binding_count, (uint8_t)ARRAY_SIZE(resp.bindings));
    for (pb_size_t i = 0; i < resp.bindings_count; i++) {
        binding_to_proto(&cfg.bindings[i], &resp.bindings[i]);
    }

    return GREMLIN_RESPONSE(get_tap_dance, resp);
}

static zmk_studio_Response set_tap_dance(const zmk_studio_Request *req) {
    const zmk_gremlin_TapDanceConfig *in = &req->subsystem.gremlin.request_type.set_tap_dance;

    struct zmk_gremlin_tap_dance_slot cfg = {0};
    cfg.enabled = in->enabled;
    cfg.tapping_term_ms = in->tapping_term_ms;
    cfg.binding_count =
        (uint8_t)MIN(in->bindings_count, (pb_size_t)ZMK_GREMLIN_MAX_TAP_DANCE_STEPS);
    for (uint8_t i = 0; i < cfg.binding_count; i++) {
        if (in->bindings[i].behavior_id >= 0) {
            int b_err = proto_to_binding(&in->bindings[i], &cfg.bindings[i]);
            if (b_err < 0) {
                return GREMLIN_RESPONSE(set_tap_dance, errno_to_gremlin_status(b_err));
            }
        } else if (in->enabled) {
            return GREMLIN_RESPONSE(
                set_tap_dance, zmk_gremlin_GremlinStatusCode_GREMLIN_STATUS_ERR_INVALID_ARG);
        }
    }

    int err = zmk_gremlin_tap_dance_set(in->index, &cfg);
    return GREMLIN_RESPONSE(set_tap_dance, errno_to_gremlin_status(err));
}

static zmk_studio_Response get_macro(const zmk_studio_Request *req) {
    uint32_t slot = req->subsystem.gremlin.request_type.get_macro;

    struct zmk_gremlin_macro_slot cfg;
    int err = zmk_gremlin_macro_get(slot, &cfg);
    if (err < 0) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    zmk_gremlin_MacroConfig resp = zmk_gremlin_MacroConfig_init_zero;
    resp.index = slot;
    resp.enabled = cfg.enabled;
    resp.wait_ms = cfg.wait_ms;
    resp.tap_ms = cfg.tap_ms;
    resp.steps_count = (pb_size_t)MIN(cfg.step_count, (uint8_t)ARRAY_SIZE(resp.steps));
    for (pb_size_t i = 0; i < resp.steps_count; i++) {
        resp.steps[i].type = (zmk_gremlin_MacroStepType)cfg.steps[i].type;
        resp.steps[i].delay_ms = cfg.steps[i].delay_ms;
        if (cfg.steps[i].type != ZMK_GREMLIN_MACRO_STEP_WAIT_MS) {
            resp.steps[i].has_binding = true;
            binding_to_proto(&cfg.steps[i].binding, &resp.steps[i].binding);
        }
    }

    return GREMLIN_RESPONSE(get_macro, resp);
}

static zmk_studio_Response set_macro(const zmk_studio_Request *req) {
    const zmk_gremlin_MacroConfig *in = &req->subsystem.gremlin.request_type.set_macro;

    struct zmk_gremlin_macro_slot cfg = {0};
    cfg.enabled = in->enabled;
    cfg.wait_ms = in->wait_ms;
    cfg.tap_ms = in->tap_ms;
    cfg.step_count = (uint8_t)MIN(in->steps_count, (pb_size_t)ZMK_GREMLIN_MAX_MACRO_STEPS);
    for (uint8_t i = 0; i < cfg.step_count; i++) {
        cfg.steps[i].type = (uint8_t)in->steps[i].type;
        cfg.steps[i].delay_ms = in->steps[i].delay_ms;
        if (cfg.steps[i].type != ZMK_GREMLIN_MACRO_STEP_WAIT_MS) {
            if (in->steps[i].has_binding && in->steps[i].binding.behavior_id >= 0) {
                int b_err = proto_to_binding(&in->steps[i].binding, &cfg.steps[i].binding);
                if (b_err < 0) {
                    return GREMLIN_RESPONSE(set_macro, errno_to_gremlin_status(b_err));
                }
            } else if (in->enabled) {
                return GREMLIN_RESPONSE(
                    set_macro, zmk_gremlin_GremlinStatusCode_GREMLIN_STATUS_ERR_INVALID_ARG);
            }
        }
    }

    int err = zmk_gremlin_macro_set(in->index, &cfg);
    return GREMLIN_RESPONSE(set_macro, errno_to_gremlin_status(err));
}

static zmk_studio_Response get_bitmap_chunk(const zmk_studio_Request *req) {
    const zmk_gremlin_GetBitmapChunkRequest *in =
        &req->subsystem.gremlin.request_type.get_bitmap_chunk;

    zmk_gremlin_BitmapChunk resp = zmk_gremlin_BitmapChunk_init_zero;
    size_t req_len = MIN((size_t)in->length, sizeof(resp.data.bytes));
    size_t actual_len = 0;
    size_t total_size = 0;
    bool is_custom = false;

    int err = gremlin_display_read_bitmap_chunk((enum gremlin_bitmap_target)in->target,
                                                (uint8_t)in->slot, in->offset, resp.data.bytes,
                                                req_len, &actual_len, &total_size, &is_custom);
    if (err < 0) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    resp.target = in->target;
    resp.slot = in->slot;
    resp.offset = in->offset;
    resp.total_size = (uint32_t)total_size;
    resp.is_custom = is_custom;
    resp.data.size = (pb_size_t)actual_len;

    return GREMLIN_RESPONSE(get_bitmap_chunk, resp);
}

static zmk_studio_Response set_bitmap_chunk(const zmk_studio_Request *req) {
    const zmk_gremlin_SetBitmapChunkRequest *in =
        &req->subsystem.gremlin.request_type.set_bitmap_chunk;

    int err = gremlin_display_write_bitmap_chunk(
        (enum gremlin_bitmap_target)in->target, (uint8_t)in->slot, in->offset, in->total_size,
        in->data.bytes, MIN((size_t)in->data.size, sizeof(in->data.bytes)));

    return GREMLIN_RESPONSE(set_bitmap_chunk, errno_to_gremlin_status(err));
}

static zmk_studio_Response clear_bitmap(const zmk_studio_Request *req) {
    const zmk_gremlin_ClearBitmapRequest *in = &req->subsystem.gremlin.request_type.clear_bitmap;

    int err = gremlin_display_clear_bitmap((enum gremlin_bitmap_target)in->target,
                                           (uint8_t)in->slot);

    return GREMLIN_RESPONSE(clear_bitmap, errno_to_gremlin_status(err));
}

static zmk_studio_Response save_settings(const zmk_studio_Request *req) {
    ARG_UNUSED(req);

    int err = 0;
    int r;

    r = zmk_activity_save_timeouts();
    if (r < 0 && err == 0) err = r;

    r = zmk_analog_stick_save_settings();
    if (r < 0 && err == 0) err = r;

    r = zmk_combo_save_settings();
    if (r < 0 && err == 0) err = r;

    r = zmk_gremlin_behaviors_save_settings();
    if (r < 0 && err == 0) err = r;

    r = gremlin_display_save_settings();
    if (r < 0 && err == 0) err = r;

    return GREMLIN_RESPONSE(save_settings, errno_to_gremlin_status(err));
}

static int gremlin_reset_all_subsystems(void) {
    int err = 0;
    int r;

    r = zmk_activity_reset_timeouts();
    if (r < 0 && err == 0) err = r;

    r = zmk_analog_stick_reset_settings();
    if (r < 0 && err == 0) err = r;

    r = zmk_combo_reset_settings();
    if (r < 0 && err == 0) err = r;

    r = zmk_gremlin_behaviors_reset_settings();
    if (r < 0 && err == 0) err = r;

    r = gremlin_display_reset_settings();
    if (r < 0 && err == 0) err = r;

    return err;
}

static zmk_studio_Response reset_gremlin_defaults(const zmk_studio_Request *req) {
    ARG_UNUSED(req);
    int err = gremlin_reset_all_subsystems();
    return GREMLIN_RESPONSE(reset_gremlin_defaults, errno_to_gremlin_status(err));
}

ZMK_RPC_SUBSYSTEM_SETTINGS_RESET(gremlin, gremlin_reset_all_subsystems);

ZMK_RPC_SUBSYSTEM_HANDLER(gremlin, get_capabilities, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(gremlin, get_sleep_config, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(gremlin, set_sleep_config, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(gremlin, get_display_config, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(gremlin, set_display_config, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(gremlin, get_stick_config, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(gremlin, set_stick_config, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(gremlin, get_stick_layer_config, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(gremlin, set_stick_layer_config, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(gremlin, get_stick_telemetry, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(gremlin, get_combo, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(gremlin, set_combo, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(gremlin, get_hold_tap, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(gremlin, set_hold_tap, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(gremlin, get_tap_dance, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(gremlin, set_tap_dance, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(gremlin, get_macro, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(gremlin, set_macro, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(gremlin, get_bitmap_chunk, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(gremlin, set_bitmap_chunk, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(gremlin, clear_bitmap, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(gremlin, save_settings, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(gremlin, reset_gremlin_defaults, ZMK_STUDIO_RPC_HANDLER_SECURED);
