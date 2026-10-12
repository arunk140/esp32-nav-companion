/*
 * SPDX-FileCopyrightText: 2015-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

/** Major version number (X.x.x) */
#define LVGL_DEMO_VERSION_MAJOR 0
/** Minor version number (x.X.x) */
#define LVGL_DEMO_VERSION_MINOR 0
/** Patch version number (x.x.X) */
#define LVGL_DEMO_VERSION_PATCH 1

/**
 * Macro to convert version number into an integer
 *
 * To be used in comparisons, such as LVGL_DEMO_VERSION >= LVGL_DEMO_VERSION_VAL(4, 0, 0)
 */
#define LVGL_DEMO_VERSION_VAL(major, minor, patch) ((major << 16) | (minor << 8) | (patch))

/**
 * Current version, as an integer
 *
 * To be used in comparisons, such as LVGL_DEMO_VERSION >= LVGL_DEMO_VERSION_VAL(4, 0, 0)
 */
#define LVGL_DEMO_VERSION LVGL_DEMO_VERSION_VAL(LVGL_DEMO_VERSION_MAJOR, \
                                                LVGL_DEMO_VERSION_MINOR, \
                                                LVGL_DEMO_VERSION_PATCH)

