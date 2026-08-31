/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * DIAGNOSTIC MINIMAL STATUS SCREEN.
 * Temporarily replaces the full custom screen (widgets + vendored NerdFonts +
 * canvas) with a solid background and one built-in-font label, to isolate a
 * boot fault: the panel powers on (static) but the firmware faults before USB
 * enumerates. If this minimal screen renders (blue background + "CORNIX OK")
 * and USB enumerates, the display driver + LVGL base + built-in fonts are all
 * fine and the fault is in the custom widgets/NerdFonts/canvas. If it still
 * shows static with no USB, the fault is in the display driver / overlay / LVGL
 * base itself. The full screen is preserved in git history.
 */

#include "custom_status_screen.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

lv_obj_t *zmk_display_status_screen(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);

    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0000FF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, "CORNIX OK");
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    return screen;
}
