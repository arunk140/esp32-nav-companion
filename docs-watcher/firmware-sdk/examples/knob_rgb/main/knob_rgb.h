/*
 * SPDX-FileCopyrightText: 2024 Seeed Tech. Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/** Major version number (X.x.x) */
#define KNOB_RGB_VERSION_MAJOR 0
/** Minor version number (x.X.x) */
#define KNOB_RGB_VERSION_MINOR 0
/** Patch version number (x.x.X) */
#define KNOB_RGB_VERSION_PATCH 1

/**
 * Macro to convert version number into an integer
 *
 * To be used in comparisons, such as KNOB_RGB_VERSION >= KNOB_RGB_VERSION_VAL(4, 0, 0)
 */
#define KNOB_RGB_VERSION_VAL(major, minor, patch) ((major << 16) | (minor << 8) | (patch))

/**
 * Current version, as an integer
 *
 * To be used in comparisons, such as KNOB_RGB_VERSION >= KNOB_RGB_VERSION_VAL(4, 0, 0)
 */
#define KNOB_RGB_VERSION KNOB_RGB_VERSION_VAL(KNOB_RGB_VERSION_MAJOR, KNOB_RGB_VERSION_MINOR, KNOB_RGB_VERSION_PATCH)
