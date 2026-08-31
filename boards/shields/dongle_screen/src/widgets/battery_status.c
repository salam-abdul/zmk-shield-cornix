/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/services/bas.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/battery.h>
#include <zmk/split/central.h>
#include <zmk/display.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/usb.h>

#include "battery_status.h"
#include "../brightness.h"

#if IS_ENABLED(CONFIG_ZMK_DONGLE_DISPLAY_DONGLE_BATTERY)
    #define SOURCE_OFFSET 1
#else
    #define SOURCE_OFFSET 0
#endif

#define BATTERY_SOURCES (ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT + SOURCE_OFFSET)

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct battery_state {
    uint8_t source;
    uint8_t level;
    bool usb_present;
};

// Rendered with a standard lv_bar gauge + percentage label per source (no canvas):
// the LVGL-8 canvas battery draw did not survive the port to LVGL 9.3 and faulted
// during render. A bar + label uses only core widgets and is robust.
struct battery_object {
    lv_obj_t *bar;
    lv_obj_t *label;
} battery_objects[BATTERY_SOURCES];

// Peripheral reconnection tracking
// ZMK sends battery events with level < 1 when peripherals disconnect
static int8_t last_battery_levels[BATTERY_SOURCES];

static void init_peripheral_tracking(void) {
    for (int i = 0; i < BATTERY_SOURCES; i++) {
        last_battery_levels[i] = -1; // -1 indicates never seen before
    }
}

static bool is_peripheral_reconnecting(uint8_t source, uint8_t new_level) {
    if (source >= BATTERY_SOURCES) {
        return false;
    }

    int8_t previous_level = last_battery_levels[source];

    // Reconnection detected if the previous level was < 1 (disconnected/unknown)
    // and the new level is a valid battery level.
    bool reconnecting = (previous_level < 1) && (new_level >= 1);

    if (reconnecting) {
        LOG_INF("Peripheral %d reconnection: %d%% -> %d%% (was %s)",
                source, previous_level, new_level,
                previous_level == -1 ? "never seen" : "disconnected");
    }

    return reconnecting;
}

static lv_color_t level_color(uint8_t level) {
    if (level < 1) {
        return lv_palette_main(LV_PALETTE_RED);
    } else if (level <= 20) {
        return lv_palette_main(LV_PALETTE_YELLOW);
    }
    return lv_palette_main(LV_PALETTE_GREEN);
}

static void set_battery_symbol(lv_obj_t *widget, struct battery_state state) {
    if (state.source >= BATTERY_SOURCES) {
        return;
    }

    bool reconnecting = is_peripheral_reconnecting(state.source, state.level);
    last_battery_levels[state.source] = state.level;

    if (reconnecting) {
#if CONFIG_DONGLE_SCREEN_IDLE_TIMEOUT_S > 0
        LOG_INF("Peripheral %d reconnected (battery: %d%%), requesting screen wake",
                state.source, state.level);
        brightness_wake_screen_on_reconnect();
#else
        LOG_INF("Peripheral %d reconnected (battery: %d%%)", state.source, state.level);
#endif
    }

    LOG_DBG("source: %d, level: %d, usb: %d", state.source, state.level, state.usb_present);

    lv_obj_t *bar = battery_objects[state.source].bar;
    lv_obj_t *label = battery_objects[state.source].label;
    lv_color_t color = level_color(state.level);

    if (state.level > 0) {
        lv_bar_set_value(bar, state.level, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, color, LV_PART_INDICATOR);
        lv_label_set_text_fmt(label, "%u%%", state.level);
        lv_obj_set_style_text_color(label, color, 0);
    } else {
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        lv_label_set_text(label, "X");
        lv_obj_set_style_text_color(label, lv_palette_main(LV_PALETTE_RED), 0);
    }

    lv_obj_clear_flag(bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
}

void battery_status_update_cb(struct battery_state state) {
    struct zmk_widget_dongle_battery_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_symbol(widget->obj, state); }
}

static struct battery_state peripheral_battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev = as_zmk_peripheral_battery_state_changed(eh);
    return (struct battery_state){
        .source = ev->source + SOURCE_OFFSET,
        .level = ev->state_of_charge,
    };
}

static struct battery_state central_battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    return (struct battery_state) {
        .source = 0,
        .level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
    };
}

static struct battery_state battery_status_get_state(const zmk_event_t *eh) {
    if (as_zmk_peripheral_battery_state_changed(eh) != NULL) {
        return peripheral_battery_status_get_state(eh);
    } else {
        return central_battery_status_get_state(eh);
    }
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_dongle_battery_status, struct battery_state,
                            battery_status_update_cb, battery_status_get_state)

ZMK_SUBSCRIPTION(widget_dongle_battery_status, zmk_peripheral_battery_state_changed);

#if IS_ENABLED(CONFIG_ZMK_DONGLE_DISPLAY_DONGLE_BATTERY)
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

ZMK_SUBSCRIPTION(widget_dongle_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_dongle_battery_status, zmk_usb_conn_state_changed);
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
#endif /* !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) */
#endif /* IS_ENABLED(CONFIG_ZMK_DONGLE_DISPLAY_DONGLE_BATTERY) */

int zmk_widget_dongle_battery_status_init(struct zmk_widget_dongle_battery_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 240, 44);
    lv_obj_set_style_bg_opa(widget->obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(widget->obj, 0, 0);
    lv_obj_set_style_pad_all(widget->obj, 0, 0);

    for (int i = 0; i < BATTERY_SOURCES; i++) {
        lv_obj_t *label = lv_label_create(widget->obj);
        lv_label_set_text(label, "--");
        lv_obj_align(label, LV_ALIGN_TOP_MID, -60 + (i * 120), 0);

        lv_obj_t *bar = lv_bar_create(widget->obj);
        lv_obj_set_size(bar, 104, 14);
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, -60 + (i * 120), -2);

        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);

        battery_objects[i] = (struct battery_object){
            .bar = bar,
            .label = label,
        };
    }

    sys_slist_append(&widgets, &widget->node);

    init_peripheral_tracking();

    widget_dongle_battery_status_init();

    return 0;
}

lv_obj_t *zmk_widget_dongle_battery_status_obj(struct zmk_widget_dongle_battery_status *widget) {
    return widget->obj;
}
