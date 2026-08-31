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

// Rendered as a single label (like the working layer widget), NOT a container
// with child objects: the LVGL-8 dongle-screen battery widget wrapped its bars
// in an lv_obj container, which faults during render under LVGL 9.3 (both the
// original canvas and an lv_bar rewrite crashed identically). A bare label shows
// each half's percentage, color-coded by the lowest known level.

// ZMK sends battery events with level < 1 when a peripheral disconnects.
// -1 = never seen.
static int8_t last_battery_levels[BATTERY_SOURCES];

static void init_peripheral_tracking(void) {
    for (int i = 0; i < BATTERY_SOURCES; i++) {
        last_battery_levels[i] = -1;
    }
}

static bool is_peripheral_reconnecting(uint8_t source, uint8_t new_level) {
    if (source >= BATTERY_SOURCES) {
        return false;
    }
    int8_t previous_level = last_battery_levels[source];
    bool reconnecting = (previous_level < 1) && (new_level >= 1);
    if (reconnecting) {
        LOG_INF("Peripheral %d reconnection: %d%% -> %d%% (was %s)",
                source, previous_level, new_level,
                previous_level == -1 ? "never seen" : "disconnected");
    }
    return reconnecting;
}

static void refresh_label(lv_obj_t *label) {
    char text[48] = "";
    int off = 0;
    lv_color_t color = lv_palette_main(LV_PALETTE_GREEN);

    for (int i = 0; i < BATTERY_SOURCES; i++) {
        int8_t lvl = last_battery_levels[i];
        const char *sep = (i == 0) ? "" : "   ";
        if (lvl < 1) {
            off += snprintf(text + off, sizeof(text) - off, "%s--", sep);
            color = lv_palette_main(LV_PALETTE_RED);
        } else {
            off += snprintf(text + off, sizeof(text) - off, "%s%d%%", sep, lvl);
            if (lvl <= 10) {
                color = lv_palette_main(LV_PALETTE_RED);
            } else if (lvl <= 20 && color.red != 0) {
                color = lv_palette_main(LV_PALETTE_YELLOW);
            }
        }
    }

    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
}

static void set_battery_symbol(lv_obj_t *label, struct battery_state state) {
    if (state.source >= BATTERY_SOURCES) {
        return;
    }

    bool reconnecting = is_peripheral_reconnecting(state.source, state.level);
    last_battery_levels[state.source] = state.level;

    if (reconnecting) {
#if CONFIG_DONGLE_SCREEN_IDLE_TIMEOUT_S > 0
        brightness_wake_screen_on_reconnect();
#endif
    }

    LOG_DBG("source: %d, level: %d, usb: %d", state.source, state.level, state.usb_present);
    refresh_label(label);
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
    init_peripheral_tracking();

    widget->obj = lv_label_create(parent);
    lv_obj_set_style_text_font(widget->obj, &lv_font_montserrat_20, 0);
    lv_label_set_text(widget->obj, "--");

    sys_slist_append(&widgets, &widget->node);

    widget_dongle_battery_status_init();

    return 0;
}

lv_obj_t *zmk_widget_dongle_battery_status_obj(struct zmk_widget_dongle_battery_status *widget) {
    return widget->obj;
}
