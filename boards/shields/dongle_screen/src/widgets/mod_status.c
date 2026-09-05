/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zmk/hid.h>
#include <lvgl.h>
#include "mod_status.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// Rebuilt for LVGL 9.3 / Zephyr 4.1:
//  - bare label (no lv_obj container, which faults during render under 9.3),
//  - built-in montserrat font (the vendored NerdFonts_40 no longer renders),
//  - readable text modifier names instead of Nerd-Font glyph icons,
//  - polled from an lv_timer (runs in the LVGL task context) rather than a
//    Zephyr k_timer ISR, so lv_label_set_text is called safely.

static struct zmk_widget_mod_status *active_widget = NULL;

static void update_mod_status(struct zmk_widget_mod_status *widget) {
    uint8_t mods = zmk_hid_get_keyboard_report()->body.modifiers;
    const char *names[4];
    int n = 0;

    if (mods & (MOD_LCTL | MOD_RCTL)) {
        names[n++] = "Ctrl";
    }
    if (mods & (MOD_LSFT | MOD_RSFT)) {
        names[n++] = "Shift";
    }
    if (mods & (MOD_LALT | MOD_RALT)) {
        names[n++] = "Alt";
    }
    if (mods & (MOD_LGUI | MOD_RGUI)) {
        names[n++] = "Gui";
    }

    char text[48] = "";
    int idx = 0;
    for (int i = 0; i < n; ++i) {
        idx += snprintf(&text[idx], sizeof(text) - idx, "%s%s", i ? "  " : "", names[i]);
    }

    lv_label_set_text(widget->label, text);
}

static void mod_status_timer_cb(lv_timer_t *timer) {
    ARG_UNUSED(timer);
    if (active_widget != NULL) {
        update_mod_status(active_widget);
    }
}

int zmk_widget_mod_status_init(struct zmk_widget_mod_status *widget, lv_obj_t *parent) {
    widget->obj = lv_label_create(parent);
    widget->label = widget->obj;

    lv_obj_set_style_text_font(widget->obj, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(widget->obj, lv_palette_main(LV_PALETTE_CYAN), 0);
    lv_label_set_text(widget->obj, "");

    active_widget = widget;
    lv_timer_create(mod_status_timer_cb, 100, NULL);

    return 0;
}

lv_obj_t *zmk_widget_mod_status_obj(struct zmk_widget_mod_status *widget) {
    return widget->obj;
}
